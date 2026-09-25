"""Outbox relay tests using in-memory fakes (tech-stack §8.2, FR-10, AC-8)."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

import pytest
from opentelemetry.sdk.metrics import MeterProvider
from opentelemetry.sdk.metrics.export import InMemoryMetricReader

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

    def count_pending(self) -> int:
        return len(self._pending)


class FakePublisher:
    def __init__(self, fail_on: set[str] | None = None) -> None:
        self.calls: list[tuple[str, str, bytes, str]] = []
        self._fail_on = fail_on or set()

    def publish(self, topic: str, key: str, payload: bytes, traceparent: str) -> None:
        if topic in self._fail_on:
            raise RuntimeError("broker down")
        self.calls.append((topic, key, payload, traceparent))


def _record(
    rid: str, topic: str = "payment.processed", created_at: datetime | None = None
) -> OutboxRecord:
    return OutboxRecord(
        id=rid,
        topic=topic,
        aggregate_id="saga-1",
        payload=b"{}",
        traceparent="tp",
        created_at=created_at,
    )


def _reader_meter() -> tuple[InMemoryMetricReader, MeterProvider]:
    reader = InMemoryMetricReader()
    provider = MeterProvider(metric_readers=[reader])
    return reader, provider


def _histogram(reader: InMemoryMetricReader, name: str):
    data = reader.get_metrics_data()
    for rm in data.resource_metrics:
        for sm in rm.scope_metrics:
            for metric in sm.metrics:
                if metric.name == name:
                    return metric
    raise AssertionError(f"metric {name!r} not found")


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


def test_drain_records_publish_lag_from_created_at() -> None:
    created = datetime.now(timezone.utc) - timedelta(seconds=3)
    repo = FakeOutboxRepo([_record("a", created_at=created)])
    publisher = FakePublisher()
    reader, provider = _reader_meter()

    Relay(repo, publisher, meter=provider.get_meter("test")).drain_once()

    metric = _histogram(reader, "outbox_publish_lag_seconds")
    points = list(metric.data.data_points)
    assert len(points) == 1
    assert points[0].count == 1
    assert points[0].sum >= 3  # lag reflects the row's age (now - created_at)


def test_outbox_pending_gauge_reflects_backlog() -> None:
    repo = FakeOutboxRepo([_record("a"), _record("b")])
    publisher = FakePublisher()
    reader, provider = _reader_meter()

    Relay(repo, publisher, meter=provider.get_meter("test"))

    metric = _histogram(reader, "outbox_pending")
    points = list(metric.data.data_points)
    assert points[-1].value == 2
