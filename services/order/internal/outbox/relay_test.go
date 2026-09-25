package outbox

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"sync"
	"testing"
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
	relay := NewRelay(repo, pub, testLogger(), 0, 0)

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
	relay := NewRelay(repo, pub, testLogger(), 0, 0)

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
