"""Transactional-outbox relay for the Payment service (tech-stack §8.2).

The relay depends only on small ``Protocol`` interfaces so it is unit-testable
with in-memory fakes (no Kafka, no Postgres).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Protocol

from opentelemetry import metrics

logger = logging.getLogger("payment.outbox")


@dataclass
class OutboxRecord:
    id: str
    topic: str
    aggregate_id: str
    payload: bytes
    traceparent: str = ""
    created_at: datetime | None = None


class OutboxRepo(Protocol):
    def fetch_pending(self, limit: int) -> list[OutboxRecord]: ...
    def mark_published(self, record_id: str) -> None: ...
    def mark_failed(self, record_id: str) -> None: ...
    def count_pending(self) -> int: ...


class Publisher(Protocol):
    def publish(self, topic: str, key: str, payload: bytes, traceparent: str) -> None: ...


class Relay:
    """Publishes PENDING outbox rows at-least-once (FR-10, FR-11, AC-8)."""

    def __init__(
        self,
        repo: OutboxRepo,
        publisher: Publisher,
        batch_size: int = 100,
        meter: metrics.Meter | None = None,
    ) -> None:
        self._repo = repo
        self._publisher = publisher
        self._batch_size = batch_size
        self._publish_lag: metrics.Histogram | None = None
        if meter is not None:
            # Outbox SLO metrics (tech-stack §9.3): backlog depth + row age at
            # publish time.
            self._publish_lag = meter.create_histogram(
                "outbox_publish_lag_seconds",
                unit="s",
                description="age of an outbox row when the relay publishes it",
            )
            meter.create_observable_gauge(
                "outbox_pending",
                callbacks=[self._observe_pending],
                description="current count of unpublished outbox rows",
            )

    def _observe_pending(
        self, _options: metrics.CallbackOptions
    ) -> list[metrics.Observation]:
        return [metrics.Observation(self._repo.count_pending())]

    def drain_once(self) -> int:
        """Publish one batch. Returns the number of rows published."""
        rows = self._repo.fetch_pending(self._batch_size)
        published = 0
        for rec in rows:
            try:
                self._publisher.publish(rec.topic, rec.aggregate_id, rec.payload, rec.traceparent)
            except Exception:  # noqa: BLE001 - relay must not crash on broker errors
                logger.exception("publish failed; leaving row pending id=%s", rec.id)
                self._repo.mark_failed(rec.id)
                raise
            self._repo.mark_published(rec.id)
            if self._publish_lag is not None and rec.created_at is not None:
                lag = (datetime.now(timezone.utc) - rec.created_at).total_seconds()
                self._publish_lag.record(lag)
            published += 1
        return published
