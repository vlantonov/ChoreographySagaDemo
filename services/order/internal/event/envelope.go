// Package event defines the JSON event envelope shared across all services
// (tech-stack §5.1). It is dependency-free so it can be unit tested in isolation.
package event

import (
	"encoding/json"
	"time"

	"github.com/google/uuid"
)

// SchemaVersion is the current envelope schema version (tech-stack §5.2).
const SchemaVersion = 1

// Envelope is the CloudEvents-inspired wrapper carried on every Kafka message.
type Envelope struct {
	EventID        string          `json:"eventId"`
	EventType      string          `json:"eventType"`
	SchemaVersion  int             `json:"schemaVersion"`
	OccurredAt     time.Time       `json:"occurredAt"`
	SagaID         string          `json:"sagaId"`
	IdempotencyKey string          `json:"idempotencyKey"`
	Traceparent    string          `json:"traceparent"`
	Data           json.RawMessage `json:"data"`
}

// New builds an envelope for the given event type and payload. The idempotency
// key defaults to a fresh UUID; traceparent carries W3C trace context (FR-23).
func New(eventType, sagaID, traceparent string, data any) (Envelope, error) {
	payload, err := json.Marshal(data)
	if err != nil {
		return Envelope{}, err
	}
	return Envelope{
		EventID:        uuid.NewString(),
		EventType:      eventType,
		SchemaVersion:  SchemaVersion,
		OccurredAt:     time.Now().UTC(),
		SagaID:         sagaID,
		IdempotencyKey: uuid.NewString(),
		Traceparent:    traceparent,
		Data:           payload,
	}, nil
}

// Marshal serializes the envelope to JSON bytes for Kafka.
func (e Envelope) Marshal() ([]byte, error) { return json.Marshal(e) }

// Parse deserializes a Kafka message body into an Envelope.
func Parse(b []byte) (Envelope, error) {
	var e Envelope
	err := json.Unmarshal(b, &e)
	return e, err
}

// DecodeData unmarshals the event-specific payload into v.
func (e Envelope) DecodeData(v any) error { return json.Unmarshal(e.Data, v) }
