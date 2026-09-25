// Package app is the Order application layer: it drives the saga state machine
// from CreateOrder and from consumed Kafka events (ARCHITECTURE §3, §5, §6, §8).
package app

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	"github.com/google/uuid"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/trace"

	"github.com/vladiant/choreography-saga/order/internal/domain"
	"github.com/vladiant/choreography-saga/order/internal/event"
	"github.com/vladiant/choreography-saga/order/internal/store"
)

// Kafka topics (tech-stack §5.3, ARCHITECTURE §9).
const (
	TopicOrderCreated      = "order.created"
	TopicOrderCancelled    = "order.cancelled"
	TopicPaymentProcessed  = "payment.processed"
	TopicPaymentFailed     = "payment.failed"
	TopicInventoryReserved = "inventory.reserved"
	TopicInventoryResvFail = "inventory.reservation_failed"
	TopicPaymentRefunded   = "payment.refunded"
	aggregateType          = "order"
	consumerName           = "order-service"
)

// ConsumedTopics are the topics the Order service subscribes to (ARCHITECTURE §2).
func ConsumedTopics() []string {
	return []string{
		TopicPaymentProcessed, TopicPaymentFailed,
		TopicInventoryReserved, TopicInventoryResvFail, TopicPaymentRefunded,
	}
}

// topicTrigger maps an inbound topic to its saga trigger.
var topicTrigger = map[string]domain.Trigger{
	TopicPaymentProcessed:  domain.TriggerPaymentProcessed,
	TopicPaymentFailed:     domain.TriggerPaymentFailed,
	TopicInventoryReserved: domain.TriggerInventoryReserved,
	TopicInventoryResvFail: domain.TriggerInventoryReservationFail,
	TopicPaymentRefunded:   domain.TriggerPaymentRefunded,
}

// Service holds the Order application dependencies.
type Service struct {
	store        *store.Store
	log          *slog.Logger
	tracer       trace.Tracer
	propagator   propagation.TextMapPropagator
	terminals    metric.Int64Counter
	sagaDuration metric.Float64Histogram
}

func New(st *store.Store, log *slog.Logger, tracer trace.Tracer, meter metric.Meter) (*Service, error) {
	terminals, err := meter.Int64Counter("saga_terminal_total",
		metric.WithDescription("orders reaching a terminal saga state"))
	if err != nil {
		return nil, err
	}
	sagaDuration, err := meter.Float64Histogram("saga_duration_seconds",
		metric.WithDescription("end-to-end saga latency from order creation to terminal state"),
		metric.WithUnit("s"))
	if err != nil {
		return nil, err
	}
	return &Service{
		store:        st,
		log:          log,
		tracer:       tracer,
		propagator:   propagation.TraceContext{},
		terminals:    terminals,
		sagaDuration: sagaDuration,
	}, nil
}

// orderCreatedData is the OrderCreated event payload consumed by Payment.
type orderCreatedData struct {
	OrderID    string  `json:"orderId"`
	CustomerID string  `json:"customerId"`
	SKU        string  `json:"sku"`
	Quantity   int32   `json:"quantity"`
	Amount     float64 `json:"amount"`
}

type orderCancelledData struct {
	OrderID string `json:"orderId"`
	Reason  string `json:"reason"`
}

// CreateOrder persists a PENDING order and enqueues OrderCreated (FR-4, FR-5).
func (s *Service) CreateOrder(ctx context.Context, customerID, sku string, qty int32, amount float64) (store.Order, error) {
	ctx, span := s.tracer.Start(ctx, "CreateOrder")
	defer span.End()

	orderID := uuid.NewString()
	data := orderCreatedData{OrderID: orderID, CustomerID: customerID, SKU: sku, Quantity: qty, Amount: amount}
	env, err := event.New("OrderCreated", orderID, s.traceparent(ctx), data)
	if err != nil {
		return store.Order{}, err
	}
	payload, err := env.Marshal()
	if err != nil {
		return store.Order{}, err
	}

	o := store.Order{ID: orderID, CustomerID: customerID, SKU: sku, Quantity: qty, Amount: amount, Status: domain.StatusPending}
	saved, err := s.store.CreateOrder(ctx, o, store.OutboxInsert{
		AggregateType: aggregateType, AggregateID: orderID,
		EventType: "OrderCreated", Topic: TopicOrderCreated,
		Payload: payload, Traceparent: env.Traceparent,
	})
	if err != nil {
		return store.Order{}, err
	}
	s.log.InfoContext(ctx, "order created", "order_id", orderID, "status", saved.Status)
	return saved, nil
}

// GetOrder returns the current saga state of an order (FR-8).
func (s *Service) GetOrder(ctx context.Context, id string) (store.Order, error) {
	return s.store.GetOrder(ctx, id)
}

// HandleEvent processes one consumed saga event and advances the state machine.
// It is the kafkax.Handler for the Order consumer.
func (s *Service) HandleEvent(ctx context.Context, topic string, value []byte, traceparent string) error {
	trigger, ok := topicTrigger[topic]
	if !ok {
		s.log.WarnContext(ctx, "ignoring unknown topic", "topic", topic)
		return nil
	}
	env, err := event.Parse(value)
	if err != nil {
		s.log.ErrorContext(ctx, "bad envelope", "topic", topic, "error", err)
		return nil // poison JSON: skip rather than block the partition
	}

	// Continue the distributed trace from the event's W3C context (FR-23, AC-6).
	ctx = s.propagator.Extract(ctx, propagation.MapCarrier{"traceparent": env.Traceparent})
	ctx, span := s.tracer.Start(ctx, "consume "+topic)
	defer span.End()

	dup, err := s.store.HandleEvent(ctx, env.IdempotencyKey, consumerName, env.SagaID,
		func(cur domain.Status) (domain.Status, []store.OutboxInsert, bool, error) {
			res, aErr := domain.Apply(cur, trigger)
			if aErr != nil {
				return cur, nil, false, aErr
			}
			var emit []store.OutboxInsert
			for _, evType := range res.Emit {
				ob, bErr := s.buildEmit(ctx, evType, env.SagaID)
				if bErr != nil {
					return cur, nil, false, bErr
				}
				emit = append(emit, ob)
			}
			return res.Next, emit, res.Changed, nil
		})
	if errors.Is(err, store.ErrNotFound) {
		s.log.WarnContext(ctx, "event for unknown order", "saga_id", env.SagaID, "topic", topic)
		return nil
	}
	if err != nil {
		if errors.Is(err, domain.ErrInvalidTransition) {
			s.log.WarnContext(ctx, "invalid transition; skipping", "saga_id", env.SagaID, "topic", topic)
			return nil
		}
		if errors.Is(err, domain.ErrPrematureEvent) {
			// Park-and-retry: the tx (incl. processed_messages insert) rolled back,
			// so do not commit the offset — redelivery re-applies it once the
			// prerequisite state is reached (ARCHITECTURE §5.1).
			s.log.InfoContext(ctx, "premature event; awaiting prerequisite, will retry",
				"saga_id", env.SagaID, "topic", topic)
			return err
		}
		return fmt.Errorf("handle %s: %w", topic, err)
	}
	if dup {
		s.log.InfoContext(ctx, "duplicate event ignored", "saga_id", env.SagaID, "topic", topic)
		return nil
	}
	s.recordTerminal(ctx, env.SagaID)
	s.log.InfoContext(ctx, "event applied", "saga_id", env.SagaID, "topic", topic)
	return nil
}

func (s *Service) buildEmit(ctx context.Context, evType, sagaID string) (store.OutboxInsert, error) {
	if evType != domain.EventOrderCancelled {
		return store.OutboxInsert{}, fmt.Errorf("unsupported emit %q", evType)
	}
	env, err := event.New("OrderCancelled", sagaID, s.traceparent(ctx),
		orderCancelledData{OrderID: sagaID, Reason: "saga compensation"})
	if err != nil {
		return store.OutboxInsert{}, err
	}
	payload, err := env.Marshal()
	if err != nil {
		return store.OutboxInsert{}, err
	}
	return store.OutboxInsert{
		AggregateType: aggregateType, AggregateID: sagaID,
		EventType: "OrderCancelled", Topic: TopicOrderCancelled,
		Payload: payload, Traceparent: env.Traceparent,
	}, nil
}

// recordTerminal counts orders that reached a terminal state for SLOs (FR-25).
func (s *Service) recordTerminal(ctx context.Context, orderID string) {
	o, err := s.store.GetOrder(ctx, orderID)
	if err != nil || !o.Status.IsTerminal() {
		return
	}
	outcome := "confirmed"
	if o.Status == domain.StatusCancelled {
		outcome = "cancelled"
	}
	s.terminals.Add(ctx, 1, metric.WithAttributes(attribute.String("outcome", outcome)))
	// End-to-end saga latency: creation → terminal transition (tech-stack §9.3).
	if !o.CreatedAt.IsZero() && !o.UpdatedAt.IsZero() {
		s.sagaDuration.Record(ctx, o.UpdatedAt.Sub(o.CreatedAt).Seconds(),
			metric.WithAttributes(attribute.String("outcome", outcome)))
	}
}

// traceparent injects the active span context into a W3C traceparent string.
func (s *Service) traceparent(ctx context.Context) string {
	carrier := propagation.MapCarrier{}
	s.propagator.Inject(ctx, carrier)
	return carrier["traceparent"]
}
