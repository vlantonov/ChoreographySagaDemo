// Package outbox implements the transactional-outbox relay (tech-stack §8.2,
// FR-9..FR-11). The relay loop depends only on small interfaces so it can be
// unit tested with in-memory fakes (no Kafka, no Postgres).
package outbox

import (
	"context"
	"log/slog"
	"time"
)

// Record is one pending outbox row (tech-stack §8.1).
type Record struct {
	ID            string
	AggregateType string
	AggregateID   string
	EventType     string
	Topic         string
	Payload       []byte
	Traceparent   string
}

// Repo reads and marks outbox rows. The SQL implementation selects PENDING rows
// with FOR UPDATE SKIP LOCKED so concurrent/restarted relays never double-send.
type Repo interface {
	FetchPending(ctx context.Context, limit int) ([]Record, error)
	MarkPublished(ctx context.Context, id string) error
	MarkFailed(ctx context.Context, id string) error
}

// Publisher sends a single record to the event backbone.
type Publisher interface {
	Publish(ctx context.Context, topic, key string, payload []byte, traceparent string) error
}

// Relay polls the outbox and publishes pending rows at-least-once.
type Relay struct {
	repo      Repo
	pub       Publisher
	log       *slog.Logger
	batchSize int
	interval  time.Duration
}

// NewRelay constructs a relay. interval<=0 defaults to 500ms; batch<=0 to 100.
func NewRelay(repo Repo, pub Publisher, log *slog.Logger, interval time.Duration, batch int) *Relay {
	if interval <= 0 {
		interval = 500 * time.Millisecond
	}
	if batch <= 0 {
		batch = 100
	}
	return &Relay{repo: repo, pub: pub, log: log, batchSize: batch, interval: interval}
}

// Run loops until ctx is cancelled, draining the outbox each tick.
func (r *Relay) Run(ctx context.Context) {
	ticker := time.NewTicker(r.interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			if n, err := r.drainOnce(ctx); err != nil {
				r.log.ErrorContext(ctx, "outbox drain failed", "error", err)
			} else if n > 0 {
				r.log.DebugContext(ctx, "outbox drained", "published", n)
			}
		}
	}
}

// drainOnce publishes one batch of pending rows. It is exported-for-test via
// DrainOnce and is the unit under test for the relay (AC-8).
func (r *Relay) drainOnce(ctx context.Context) (int, error) {
	rows, err := r.repo.FetchPending(ctx, r.batchSize)
	if err != nil {
		return 0, err
	}
	published := 0
	for _, rec := range rows {
		if err := r.pub.Publish(ctx, rec.Topic, rec.AggregateID, rec.Payload, rec.Traceparent); err != nil {
			// Leave the row PENDING; a later tick retries (at-least-once).
			if mErr := r.repo.MarkFailed(ctx, rec.ID); mErr != nil {
				r.log.ErrorContext(ctx, "mark failed errored", "id", rec.ID, "error", mErr)
			}
			return published, err
		}
		if err := r.repo.MarkPublished(ctx, rec.ID); err != nil {
			// Published but not marked: duplicate on restart, absorbed by idempotency.
			return published, err
		}
		published++
	}
	return published, nil
}

// DrainOnce runs a single drain cycle; used by tests and startup catch-up.
func (r *Relay) DrainOnce(ctx context.Context) (int, error) { return r.drainOnce(ctx) }
