// Package outbox implements the transactional-outbox relay (tech-stack §8.2,
// FR-9..FR-11). The relay loop depends only on small interfaces so it can be
// unit tested with in-memory fakes (no Kafka, no Postgres).
package outbox

import (
	"context"
	"log/slog"
	"time"

	"go.opentelemetry.io/otel/metric"
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
	CreatedAt     time.Time
}

// Repo reads and marks outbox rows. The SQL implementation selects PENDING rows
// with FOR UPDATE SKIP LOCKED so concurrent/restarted relays never double-send.
type Repo interface {
	FetchPending(ctx context.Context, limit int) ([]Record, error)
	MarkPublished(ctx context.Context, id string) error
	MarkFailed(ctx context.Context, id string) error
	// CountPending returns the number of unpublished outbox rows, backing the
	// outbox_pending SLO gauge (tech-stack §9.3).
	CountPending(ctx context.Context) (int64, error)
}

// Publisher sends a single record to the event backbone.
type Publisher interface {
	Publish(ctx context.Context, topic, key string, payload []byte, traceparent string) error
}

// Relay polls the outbox and publishes pending rows at-least-once.
type Relay struct {
	repo       Repo
	pub        Publisher
	log        *slog.Logger
	batchSize  int
	interval   time.Duration
	publishLag metric.Float64Histogram
}

// NewRelay constructs a relay. interval<=0 defaults to 500ms; batch<=0 to 100.
// It also registers the outbox SLO metrics (tech-stack §9.3): an observable
// outbox_pending gauge (backlog depth) and an outbox_publish_lag_seconds
// histogram (row age at publish time).
func NewRelay(repo Repo, pub Publisher, log *slog.Logger, meter metric.Meter, interval time.Duration, batch int) (*Relay, error) {
	if interval <= 0 {
		interval = 500 * time.Millisecond
	}
	if batch <= 0 {
		batch = 100
	}
	publishLag, err := meter.Float64Histogram("outbox_publish_lag_seconds",
		metric.WithDescription("age of an outbox row when the relay publishes it"),
		metric.WithUnit("s"))
	if err != nil {
		return nil, err
	}
	pending, err := meter.Int64ObservableGauge("outbox_pending",
		metric.WithDescription("current count of unpublished outbox rows"))
	if err != nil {
		return nil, err
	}
	if _, err := meter.RegisterCallback(func(ctx context.Context, o metric.Observer) error {
		n, cErr := repo.CountPending(ctx)
		if cErr != nil {
			return cErr
		}
		o.ObserveInt64(pending, n)
		return nil
	}, pending); err != nil {
		return nil, err
	}
	return &Relay{repo: repo, pub: pub, log: log, batchSize: batch, interval: interval, publishLag: publishLag}, nil
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
		if !rec.CreatedAt.IsZero() {
			r.publishLag.Record(ctx, time.Since(rec.CreatedAt).Seconds())
		}
		published++
	}
	return published, nil
}

// DrainOnce runs a single drain cycle; used by tests and startup catch-up.
func (r *Relay) DrainOnce(ctx context.Context) (int, error) { return r.drainOnce(ctx) }
