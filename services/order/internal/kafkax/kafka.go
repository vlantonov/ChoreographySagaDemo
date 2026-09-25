// Package kafkax provides thin franz-go producer/consumer wrappers with manual
// offset commit (at-least-once, tech-stack §3) and W3C traceparent propagation
// through Kafka headers (FR-23).
package kafkax

import (
	"context"
	"strings"

	"github.com/twmb/franz-go/pkg/kgo"
)

const traceparentHeader = "traceparent"

// Producer publishes outbox rows to Kafka and satisfies outbox.Publisher.
type Producer struct{ cl *kgo.Client }

func NewProducer(brokers string) (*Producer, error) {
	cl, err := kgo.NewClient(
		kgo.SeedBrokers(strings.Split(brokers, ",")...),
		kgo.ProducerLinger(0),
		kgo.RequiredAcks(kgo.AllISRAcks()),
	)
	if err != nil {
		return nil, err
	}
	return &Producer{cl: cl}, nil
}

// Publish sends a record synchronously and returns the broker error, if any.
func (p *Producer) Publish(ctx context.Context, topic, key string, payload []byte, traceparent string) error {
	rec := &kgo.Record{
		Topic: topic,
		Key:   []byte(key),
		Value: payload,
	}
	if traceparent != "" {
		rec.Headers = append(rec.Headers, kgo.RecordHeader{Key: traceparentHeader, Value: []byte(traceparent)})
	}
	return p.cl.ProduceSync(ctx, rec).FirstErr()
}

func (p *Producer) Close() { p.cl.Close() }

// Handler processes a single consumed record. Returning an error prevents the
// offset commit so the message is redelivered (idempotency absorbs duplicates).
type Handler func(ctx context.Context, topic string, value []byte, traceparent string) error

// Consumer is a manual-commit consumer group over the given topics.
type Consumer struct {
	cl     *kgo.Client
	topics []string
}

func NewConsumer(brokers, group string, topics []string) (*Consumer, error) {
	cl, err := kgo.NewClient(
		kgo.SeedBrokers(strings.Split(brokers, ",")...),
		kgo.ConsumerGroup(group),
		kgo.ConsumeTopics(topics...),
		kgo.DisableAutoCommit(),
	)
	if err != nil {
		return nil, err
	}
	return &Consumer{cl: cl, topics: topics}, nil
}

// Run polls and dispatches records until ctx is cancelled, committing offsets
// only after a handler succeeds (at-least-once).
func (c *Consumer) Run(ctx context.Context, h Handler) {
	for {
		if ctx.Err() != nil {
			return
		}
		fetches := c.cl.PollFetches(ctx)
		if fetches.IsClientClosed() {
			return
		}
		var failed bool
		fetches.EachRecord(func(r *kgo.Record) {
			if failed {
				return
			}
			tp := headerValue(r, traceparentHeader)
			if err := h(ctx, r.Topic, r.Value, tp); err != nil {
				failed = true // stop; do not commit so redelivery occurs
			}
		})
		if !failed {
			if err := c.cl.CommitUncommittedOffsets(ctx); err != nil {
				return
			}
		}
	}
}

func (c *Consumer) Close() { c.cl.Close() }

func headerValue(r *kgo.Record, key string) string {
	for _, h := range r.Headers {
		if h.Key == key {
			return string(h.Value)
		}
	}
	return ""
}
