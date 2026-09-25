"""JSON event envelope shared across services (tech-stack §5.1).

Pure/stdlib-only so it can be unit tested without Kafka or a database.
"""

from __future__ import annotations

import json
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

SCHEMA_VERSION = 1


@dataclass
class Envelope:
    event_type: str
    saga_id: str
    data: dict[str, Any]
    traceparent: str = ""
    event_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    idempotency_key: str = field(default_factory=lambda: str(uuid.uuid4()))
    schema_version: int = SCHEMA_VERSION
    occurred_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())

    def to_json(self) -> bytes:
        return json.dumps(
            {
                "eventId": self.event_id,
                "eventType": self.event_type,
                "schemaVersion": self.schema_version,
                "occurredAt": self.occurred_at,
                "sagaId": self.saga_id,
                "idempotencyKey": self.idempotency_key,
                "traceparent": self.traceparent,
                "data": self.data,
            }
        ).encode("utf-8")


def parse(raw: bytes | str) -> Envelope:
    """Deserialize a Kafka message body into an Envelope."""
    obj = json.loads(raw)
    return Envelope(
        event_id=obj.get("eventId", ""),
        event_type=obj.get("eventType", ""),
        schema_version=obj.get("schemaVersion", SCHEMA_VERSION),
        occurred_at=obj.get("occurredAt", ""),
        saga_id=obj.get("sagaId", ""),
        idempotency_key=obj.get("idempotencyKey", ""),
        traceparent=obj.get("traceparent", ""),
        data=obj.get("data", {}),
    )
