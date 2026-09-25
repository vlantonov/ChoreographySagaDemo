// Package domain holds the Order aggregate and its saga state machine.
//
// It has no external dependencies so the state-machine rules can be unit tested
// without a database, Kafka, or gRPC (ARCHITECTURE §3, §5).
package domain

import "errors"

// Status is the order saga state (ARCHITECTURE §5).
type Status string

const (
	StatusPending      Status = "PENDING"
	StatusPaymentOK    Status = "PAYMENT_OK"
	StatusConfirmed    Status = "CONFIRMED"
	StatusCompensating Status = "COMPENSATING"
	StatusCancelled    Status = "CANCELLED"
)

// IsTerminal reports whether the saga has reached a final state (FR-6, FR-7).
func (s Status) IsTerminal() bool {
	return s == StatusConfirmed || s == StatusCancelled
}

// Trigger is an inbound saga event type that the Order service consumes.
type Trigger string

const (
	TriggerPaymentProcessed         Trigger = "PaymentProcessed"
	TriggerPaymentFailed            Trigger = "PaymentFailed"
	TriggerInventoryReserved        Trigger = "InventoryReserved"
	TriggerInventoryReservationFail Trigger = "InventoryReservationFailed"
	TriggerPaymentRefunded          Trigger = "PaymentRefunded"
)

// EventOrderCancelled is emitted when the saga terminates in compensation.
const EventOrderCancelled = "OrderCancelled"

// ErrInvalidTransition indicates a trigger that is not defined for the state and
// is not a benign duplicate. Callers treat it as a poison message.
var ErrInvalidTransition = errors.New("invalid saga transition")

// Result describes the outcome of applying a trigger to a status.
type Result struct {
	Next    Status
	Changed bool
	// Emit lists compensation event types to publish via the outbox.
	Emit []string
}

// Apply runs the state machine. It is idempotent: replaying a trigger that has
// already advanced the saga (or hitting a terminal state) is a no-op rather than
// an error, so duplicate Kafka deliveries are absorbed (FR-12, AC-9).
func Apply(cur Status, t Trigger) (Result, error) {
	switch cur {
	case StatusPending:
		switch t {
		case TriggerPaymentProcessed:
			return Result{Next: StatusPaymentOK, Changed: true}, nil
		case TriggerPaymentFailed:
			return Result{Next: StatusCancelled, Changed: true, Emit: []string{EventOrderCancelled}}, nil
		case TriggerInventoryReservationFail:
			// No payment succeeded yet; cancel directly.
			return Result{Next: StatusCancelled, Changed: true, Emit: []string{EventOrderCancelled}}, nil
		}
	case StatusPaymentOK:
		switch t {
		case TriggerInventoryReserved:
			return Result{Next: StatusConfirmed, Changed: true}, nil
		case TriggerInventoryReservationFail:
			// Wait for payment.refunded before cancelling (reverse-order rollback).
			return Result{Next: StatusCompensating, Changed: true}, nil
		case TriggerPaymentProcessed:
			return Result{Next: StatusPaymentOK, Changed: false}, nil // duplicate
		}
	case StatusCompensating:
		switch t {
		case TriggerPaymentRefunded:
			return Result{Next: StatusCancelled, Changed: true, Emit: []string{EventOrderCancelled}}, nil
		case TriggerInventoryReservationFail:
			return Result{Next: StatusCompensating, Changed: false}, nil // duplicate
		}
	case StatusConfirmed, StatusCancelled:
		// Terminal: ignore any late/duplicate event idempotently.
		return Result{Next: cur, Changed: false}, nil
	}
	return Result{Next: cur, Changed: false}, ErrInvalidTransition
}
