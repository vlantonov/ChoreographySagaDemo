package domain

import "testing"

func TestApply_HappyPath(t *testing.T) {
	res, err := Apply(StatusPending, TriggerPaymentProcessed)
	if err != nil || res.Next != StatusPaymentOK || !res.Changed {
		t.Fatalf("pending+PaymentProcessed => %+v err=%v", res, err)
	}
	res, err = Apply(StatusPaymentOK, TriggerInventoryReserved)
	if err != nil || res.Next != StatusConfirmed || !res.Changed {
		t.Fatalf("paymentOK+InventoryReserved => %+v err=%v", res, err)
	}
	if !res.Next.IsTerminal() {
		t.Fatalf("CONFIRMED must be terminal")
	}
}

func TestApply_PaymentFailureCancels(t *testing.T) {
	res, err := Apply(StatusPending, TriggerPaymentFailed)
	if err != nil || res.Next != StatusCancelled || !res.Changed {
		t.Fatalf("pending+PaymentFailed => %+v err=%v", res, err)
	}
	if len(res.Emit) != 1 || res.Emit[0] != EventOrderCancelled {
		t.Fatalf("expected OrderCancelled emit, got %v", res.Emit)
	}
}

func TestApply_InventoryFailureCompensationChain(t *testing.T) {
	res, _ := Apply(StatusPaymentOK, TriggerInventoryReservationFail)
	if res.Next != StatusCompensating || !res.Changed || len(res.Emit) != 0 {
		t.Fatalf("paymentOK+InvFail => %+v (should wait for refund)", res)
	}
	res, _ = Apply(StatusCompensating, TriggerPaymentRefunded)
	if res.Next != StatusCancelled || !res.Changed || res.Emit[0] != EventOrderCancelled {
		t.Fatalf("compensating+Refunded => %+v", res)
	}
}

func TestApply_IdempotentDuplicatesAndTerminals(t *testing.T) {
	// Duplicate PaymentProcessed in PAYMENT_OK is a no-op (AC-9).
	res, err := Apply(StatusPaymentOK, TriggerPaymentProcessed)
	if err != nil || res.Changed {
		t.Fatalf("duplicate should be no-op, got %+v err=%v", res, err)
	}
	// Any event on a terminal state is ignored.
	for _, tr := range []Trigger{TriggerPaymentFailed, TriggerInventoryReserved, TriggerPaymentRefunded} {
		res, err := Apply(StatusConfirmed, tr)
		if err != nil || res.Changed || res.Next != StatusConfirmed {
			t.Fatalf("terminal+%s should be no-op, got %+v err=%v", tr, res, err)
		}
	}
}

func TestApply_InvalidTransition(t *testing.T) {
	if _, err := Apply(StatusPaymentOK, TriggerPaymentFailed); err != ErrInvalidTransition {
		t.Fatalf("expected ErrInvalidTransition, got %v", err)
	}
}
