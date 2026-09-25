// Package store is the PostgreSQL repository for the Order service: orders, the
// transactional outbox, and the processed_messages idempotency table
// (tech-stack §4, §8; ARCHITECTURE §4.1).
package store

import (
	"context"
	"errors"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/vladiant/choreography-saga/order/internal/domain"
	"github.com/vladiant/choreography-saga/order/internal/outbox"
)

// ErrNotFound is returned when an order id does not exist.
var ErrNotFound = errors.New("order not found")

type Store struct{ pool *pgxpool.Pool }

func New(pool *pgxpool.Pool) *Store { return &Store{pool: pool} }

// Order is the persisted order aggregate.
type Order struct {
	ID         string
	CustomerID string
	SKU        string
	Quantity   int32
	Amount     float64
	Status     domain.Status
}

// OutboxInsert is an event to enqueue transactionally with a business write.
type OutboxInsert struct {
	AggregateType string
	AggregateID   string
	EventType     string
	Topic         string
	Payload       []byte
	Traceparent   string
}

// CreateOrder persists a PENDING order and its OrderCreated event in one local
// transaction (FR-9). Returns the stored order.
func (s *Store) CreateOrder(ctx context.Context, o Order, ob OutboxInsert) (Order, error) {
	err := pgx.BeginFunc(ctx, s.pool, func(tx pgx.Tx) error {
		_, err := tx.Exec(ctx, `
			INSERT INTO orders (id, customer_id, sku, quantity, amount, status)
			VALUES ($1, $2, $3, $4, $5, $6)`,
			o.ID, o.CustomerID, o.SKU, o.Quantity, o.Amount, o.Status)
		if err != nil {
			return err
		}
		return insertOutbox(ctx, tx, ob)
	})
	if err != nil {
		return Order{}, err
	}
	return o, nil
}

// GetOrder loads an order by id.
func (s *Store) GetOrder(ctx context.Context, id string) (Order, error) {
	var o Order
	err := s.pool.QueryRow(ctx, `
		SELECT id, customer_id, sku, quantity, amount, status
		FROM orders WHERE id = $1`, id).
		Scan(&o.ID, &o.CustomerID, &o.SKU, &o.Quantity, &o.Amount, &o.Status)
	if errors.Is(err, pgx.ErrNoRows) {
		return Order{}, ErrNotFound
	}
	return o, err
}

// EventTxFunc receives the loaded order status and returns the next status and
// any compensation events to enqueue. Returning changed=false makes the handler
// a no-op (used for duplicates / terminal states).
type EventTxFunc func(cur domain.Status) (next domain.Status, emit []OutboxInsert, changed bool, err error)

// HandleEvent processes a consumed Kafka event idempotently (FR-12, FR-13). It
// records the idempotency key, applies the state transition, and enqueues any
// compensation outbox events atomically. Returns true if the message was a
// duplicate and skipped.
func (s *Store) HandleEvent(ctx context.Context, idemKey, consumer, orderID string, fn EventTxFunc) (bool, error) {
	duplicate := false
	err := pgx.BeginFunc(ctx, s.pool, func(tx pgx.Tx) error {
		tag, err := tx.Exec(ctx, `
			INSERT INTO processed_messages (idempotency_key, consumer)
			VALUES ($1, $2) ON CONFLICT DO NOTHING`, idemKey, consumer)
		if err != nil {
			return err
		}
		if tag.RowsAffected() == 0 {
			duplicate = true
			return nil // already processed; commit the (empty) marker read
		}

		var cur domain.Status
		err = tx.QueryRow(ctx, `SELECT status FROM orders WHERE id = $1 FOR UPDATE`, orderID).Scan(&cur)
		if errors.Is(err, pgx.ErrNoRows) {
			return ErrNotFound
		}
		if err != nil {
			return err
		}

		next, emit, changed, err := fn(cur)
		if err != nil {
			return err
		}
		if !changed {
			return nil
		}
		if _, err := tx.Exec(ctx,
			`UPDATE orders SET status = $2, updated_at = now() WHERE id = $1`, orderID, next); err != nil {
			return err
		}
		for _, ob := range emit {
			if err := insertOutbox(ctx, tx, ob); err != nil {
				return err
			}
		}
		return nil
	})
	return duplicate, err
}

func insertOutbox(ctx context.Context, tx pgx.Tx, ob OutboxInsert) error {
	_, err := tx.Exec(ctx, `
		INSERT INTO outbox (aggregate_type, aggregate_id, event_type, topic, payload, headers, status)
		VALUES ($1, $2, $3, $4, $5, jsonb_build_object('traceparent', $6::text), 'PENDING')`,
		ob.AggregateType, ob.AggregateID, ob.EventType, ob.Topic, ob.Payload, ob.Traceparent)
	return err
}

// --- outbox.Repo implementation (relay side) ---

// FetchPending selects PENDING rows with FOR UPDATE SKIP LOCKED so a restarted
// or concurrent relay never double-dispatches (tech-stack §8.2, AC-8).
func (s *Store) FetchPending(ctx context.Context, limit int) ([]outbox.Record, error) {
	var recs []outbox.Record
	err := pgx.BeginFunc(ctx, s.pool, func(tx pgx.Tx) error {
		rows, err := tx.Query(ctx, `
			SELECT id, aggregate_type, aggregate_id, event_type, topic, payload,
			       COALESCE(headers->>'traceparent', '')
			FROM outbox
			WHERE status = 'PENDING'
			ORDER BY created_at
			FOR UPDATE SKIP LOCKED
			LIMIT $1`, limit)
		if err != nil {
			return err
		}
		defer rows.Close()
		for rows.Next() {
			var r outbox.Record
			if err := rows.Scan(&r.ID, &r.AggregateType, &r.AggregateID,
				&r.EventType, &r.Topic, &r.Payload, &r.Traceparent); err != nil {
				return err
			}
			recs = append(recs, r)
		}
		return rows.Err()
	})
	return recs, err
}

// MarkPublished flags a row as dispatched after the broker acks (FR-10).
func (s *Store) MarkPublished(ctx context.Context, id string) error {
	_, err := s.pool.Exec(ctx,
		`UPDATE outbox SET status = 'PUBLISHED', published_at = now() WHERE id = $1`, id)
	return err
}

// MarkFailed increments the retry counter; the row stays PENDING for retry.
func (s *Store) MarkFailed(ctx context.Context, id string) error {
	_, err := s.pool.Exec(ctx,
		`UPDATE outbox SET attempts = attempts + 1 WHERE id = $1`, id)
	return err
}
