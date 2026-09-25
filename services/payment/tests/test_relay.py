"""Outbox relay tests using in-memory fakes (tech-stack §8.2, FR-10, AC-8)."""

from __future__ import annotations

import pytest

from payment.outbox import OutboxRecord, Relay


class FakeOutboxRepo:
    def __init__(self, records: list[OutboxRecord]) -> None:
        self._pending = list(records)
        self.published: list[str] = []
        self.failed: list[str] = []

    def fetch_pending(self, limit: int) -> list[OutboxRecord]:
        batch = self._pending[:limit]
        self._pending = self._pending[limit:]
        return batch

    def mark_published(self, record_id: str) -> None:
        self.published.append(record_id)

    def mark_failed(self, record_id: str) -> None:
        self.failed.append(record_id)


class FakePublisher:
    def __init__(self, fail_on: set[str] | None = None) -> None:
        self.calls: list[tuple[str, str, bytes, str]] = []
        self._fail_on = fail_on or set()

    def publish(self, topic: str, key: str, payload: bytes, traceparent: str) -> None:
        if topic in self._fail_on:
            raise RuntimeError("broker down")
        self.calls.append((topic, key, payload, traceparent))


def _record(rid: str, topic: str = "payment.processed") -> OutboxRecord:
    return OutboxRecord(id=rid, topic=topic, aggregate_id="saga-1", payload=b"{}", traceparent="tp")


def test_drain_publishes_and_marks_published() -> None:
    repo = FakeOutboxRepo([_record("a"), _record("b")])
    publisher = FakePublisher()

    published = Relay(repo, publisher).drain_once()

    assert published == 2
    assert repo.published == ["a", "b"]
    assert [c[1] for c in publisher.calls] == ["saga-1", "saga-1"]


def test_drain_marks_failed_and_reraises_on_broker_error() -> None:
    repo = FakeOutboxRepo([_record("a", topic="payment.failed")])
    publisher = FakePublisher(fail_on={"payment.failed"})

    with pytest.raises(RuntimeError):
        Relay(repo, publisher).drain_once()

    assert repo.failed == ["a"]
    assert repo.published == []  # row stays PENDING for retry (at-least-once)
