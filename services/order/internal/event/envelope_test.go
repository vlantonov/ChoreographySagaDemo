package event

import "testing"

func TestEnvelopeRoundTrip(t *testing.T) {
	type payload struct {
		OrderID string `json:"orderId"`
	}
	tp := "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"
	env, err := New("OrderCreated", "saga-1", tp, payload{OrderID: "saga-1"})
	if err != nil {
		t.Fatal(err)
	}
	if env.SchemaVersion != SchemaVersion {
		t.Fatalf("schema version = %d", env.SchemaVersion)
	}
	if env.Traceparent != tp {
		t.Fatalf("traceparent not preserved")
	}
	b, err := env.Marshal()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Parse(b)
	if err != nil {
		t.Fatal(err)
	}
	if got.EventType != "OrderCreated" || got.SagaID != "saga-1" || got.Traceparent != tp {
		t.Fatalf("round trip mismatch: %+v", got)
	}
	var p payload
	if err := got.DecodeData(&p); err != nil || p.OrderID != "saga-1" {
		t.Fatalf("data decode failed: %v %+v", err, p)
	}
}
