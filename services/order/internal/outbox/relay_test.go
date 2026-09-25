package outbox

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"

	"go.opentelemetry.io/otel/metric"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/metric/metricdata"
)

// fakeRepo is an in-memory outbox for relay tests (no Postgres).
type fakeRepo struct {
	mu        sync.Mutex
	pending   []Record
	published map[string]bool
	failed    map[string]int
}

func newFakeRepo(recs ...Record) *fakeRepo {
	return &fakeRepo{pending: recs, published: map[string]bool{}, failed: map[string]int{}}
}

func (f *fakeRepo) FetchPending(_ context.Context, limit int) ([]Record, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []Record
	for _, r := range f.pending {
		if f.published[r.ID] {
			continue
		}
		out = append(out, r)
		if len(out) >= limit {
			break
		}
	}
	return out, nil
}

func (f *fakeRepo) MarkPublished(_ context.Context, id string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.published[id] = true
	return nil
}

func (f *fakeRepo) MarkFailed(_ context.Context, id string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.failed[id]++
	return nil
}

func (f *fakeRepo) CountPending(_ context.Context) (int64, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	var n int64
	for _, r := range f.pending {
		if !f.published[r.ID] {
			n++
		}
	}
	return n, nil
}

// testMeter returns a meter backed by a manual reader so tests can collect the
// metrics the relay records.
func testMeter() (metric.Meter, *sdkmetric.ManualReader) {
	reader := sdkmetric.NewManualReader()
	mp := sdkmetric.NewMeterProvider(sdkmetric.WithReader(reader))
	return mp.Meter("test"), reader
}

// newTestRelay builds a relay with a manual-reader meter for assertions.
func newTestRelay(t *testing.T, repo Repo, pub Publisher) (*Relay, *sdkmetric.ManualReader) {
	t.Helper()
	meter, reader := testMeter()
	relay, err := NewRelay(repo, pub, testLogger(), meter, 0, 0)
	if err != nil {
		t.Fatalf("NewRelay: %v", err)
	}
	return relay, reader
}

type fakePublisher struct {
	mu      sync.Mutex
	sent    []string
	failOn  string
	failErr error
}

func (p *fakePublisher) Publish(_ context.Context, _, key string, _ []byte, _ string) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if key == p.failOn {
		return p.failErr
	}
	p.sent = append(p.sent, key)
	return nil
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestRelay_DrainPublishesAndMarks(t *testing.T) {
	repo := newFakeRepo(
		Record{ID: "1", Topic: "order.created", AggregateID: "a", Payload: []byte(`{}`)},
		Record{ID: "2", Topic: "order.created", AggregateID: "b", Payload: []byte(`{}`)},
	)
	pub := &fakePublisher{}
	relay, _ := newTestRelay(t, repo, pub)

	n, err := relay.DrainOnce(context.Background())
	if err != nil || n != 2 {
		t.Fatalf("drain => n=%d err=%v", n, err)
	}
	if !repo.published["1"] || !repo.published["2"] {
		t.Fatalf("rows not marked published: %+v", repo.published)
	}
	if len(pub.sent) != 2 {
		t.Fatalf("publisher got %d records", len(pub.sent))
	}
	// Draining again yields nothing (already published) — no double dispatch.
	n, _ = relay.DrainOnce(context.Background())
	if n != 0 {
		t.Fatalf("second drain published %d, want 0", n)
	}
}

func TestRelay_PublishFailureKeepsRowPending(t *testing.T) {
	repo := newFakeRepo(Record{ID: "1", AggregateID: "a", Payload: []byte(`{}`)})
	pub := &fakePublisher{failOn: "a", failErr: errors.New("broker down")}
	relay, _ := newTestRelay(t, repo, pub)

	if _, err := relay.DrainOnce(context.Background()); err == nil {
		t.Fatal("expected publish error")
	}
	if repo.published["1"] {
		t.Fatal("failed row must not be marked published (at-least-once)")
	}
	if repo.failed["1"] == 0 {
		t.Fatal("failed row should increment attempts")
	}
}

// histogramSum finds the named float histogram in the collected metrics and
// returns its aggregated sum and total count.
func histogramSum(t *testing.T, rm metricdata.ResourceMetrics, name string) (float64, uint64) {
	t.Helper()
	for _, sm := range rm.ScopeMetrics {
		for _, m := range sm.Metrics {
			if m.Name != name {
				continue
			}
			hist, ok := m.Data.(metricdata.Histogram[float64])
			if !ok {
				t.Fatalf("%s is not a float64 histogram", name)
			}
			var sum float64
			var count uint64
			for _, dp := range hist.DataPoints {
				sum += dp.Sum
				count += dp.Count
			}
			return sum, count
		}
	}
	t.Fatalf("histogram %q not found", name)
	return 0, 0
}

// gaugeValue finds the named int observable gauge and returns its last value.
func gaugeValue(t *testing.T, rm metricdata.ResourceMetrics, name string) int64 {
	t.Helper()
	for _, sm := range rm.ScopeMetrics {
		for _, m := range sm.Metrics {
			if m.Name != name {
				continue
			}
			g, ok := m.Data.(metricdata.Gauge[int64])
			if !ok {
				t.Fatalf("%s is not an int64 gauge", name)
			}
			if len(g.DataPoints) == 0 {
				t.Fatalf("gauge %q has no data points", name)
			}
			return g.DataPoints[len(g.DataPoints)-1].Value
		}
	}
	t.Fatalf("gauge %q not found", name)
	return 0
}

func TestRelay_RecordsPublishLagFromCreatedAt(t *testing.T) {
	created := time.Now().Add(-3 * time.Second)
	repo := newFakeRepo(
		Record{ID: "1", Topic: "order.created", AggregateID: "a", Payload: []byte(`{}`), CreatedAt: created},
	)
	pub := &fakePublisher{}
	relay, reader := newTestRelay(t, repo, pub)

	if _, err := relay.DrainOnce(context.Background()); err != nil {
		t.Fatalf("drain: %v", err)
	}

	var rm metricdata.ResourceMetrics
	if err := reader.Collect(context.Background(), &rm); err != nil {
		t.Fatalf("collect: %v", err)
	}
	sum, count := histogramSum(t, rm, "outbox_publish_lag_seconds")
	if count != 1 {
		t.Fatalf("publish-lag count = %d, want 1", count)
	}
	if sum < 3 {
		t.Fatalf("publish-lag sum = %.2fs, want >= 3s (age of created_at)", sum)
	}
}

func TestRelay_OutboxPendingGaugeReflectsBacklog(t *testing.T) {
	repo := newFakeRepo(
		Record{ID: "1", Topic: "order.created", AggregateID: "a", Payload: []byte(`{}`)},
		Record{ID: "2", Topic: "order.created", AggregateID: "b", Payload: []byte(`{}`)},
	)
	pub := &fakePublisher{}
	_, reader := newTestRelay(t, repo, pub)

	var rm metricdata.ResourceMetrics
	if err := reader.Collect(context.Background(), &rm); err != nil {
		t.Fatalf("collect: %v", err)
	}
	if got := gaugeValue(t, rm, "outbox_pending"); got != 2 {
		t.Fatalf("outbox_pending = %d, want 2", got)
	}
}
