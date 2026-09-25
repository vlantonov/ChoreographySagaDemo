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

func TestApply_PrematureInventoryReserved(t *testing.T) {
	res, err := Apply(StatusPending, TriggerInventoryReserved)
	if err != ErrPrematureEvent {
		t.Fatalf("expected ErrPrematureEvent, got %v", err)
	}
	if res.Next != StatusPending || res.Changed {
		t.Fatalf("state must be unchanged, got %+v", res)
	}
}

func TestApply_PrematurePaymentRefunded(t *testing.T) {
	if res, err := Apply(StatusPending, TriggerPaymentRefunded); err != ErrPrematureEvent || res.Changed {
		t.Fatalf("pending+PaymentRefunded => %+v err=%v (want ErrPrematureEvent, unchanged)", res, err)
	}
	if res, err := Apply(StatusPaymentOK, TriggerPaymentRefunded); err != ErrPrematureEvent || res.Changed {
		t.Fatalf("paymentOK+PaymentRefunded => %+v err=%v (want ErrPrematureEvent, unchanged)", res, err)
	}
}

func TestApply_ReorderThenPrerequisiteReachesConfirmed(t *testing.T) {
	// D1 reorder: InventoryReserved arrives while still PENDING and is parked.
	if _, err := Apply(StatusPending, TriggerInventoryReserved); err != ErrPrematureEvent {
		t.Fatalf("premature InventoryReserved should be parked, got err=%v", err)
	}
	// PaymentProcessed advances the prerequisite state.
	res, err := Apply(StatusPending, TriggerPaymentProcessed)
	if err != nil || res.Next != StatusPaymentOK || !res.Changed {
		t.Fatalf("pending+PaymentProcessed => %+v err=%v", res, err)
	}
	// Redelivered InventoryReserved now confirms the order (terminal).
	res, err = Apply(res.Next, TriggerInventoryReserved)
	if err != nil || res.Next != StatusConfirmed || !res.Changed || !res.Next.IsTerminal() {
		t.Fatalf("redelivered InventoryReserved => %+v err=%v (want CONFIRMED)", res, err)
	}
}
