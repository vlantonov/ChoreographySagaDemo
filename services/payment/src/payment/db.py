"""PostgreSQL persistence for the Payment service (tech-stack §4, §8).

Uses SQLAlchemy 2.0 Core over psycopg 3. Provides the payments repository plus
the transactional outbox and processed_messages idempotency store, and
implements the ``OutboxRepo`` protocol used by the relay.
"""

from __future__ import annotations

from dataclasses import dataclass

from sqlalchemy import Engine, create_engine, text

from payment.outbox import OutboxRecord

REFUND_CONSUMER = "payment-reservation-failed"


@dataclass
class OutboxInsert:
    aggregate_type: str
    aggregate_id: str
    event_type: str
    topic: str
    payload: bytes
    traceparent: str = ""


class Database:
    def __init__(self, url: str) -> None:
        self._engine: Engine = create_engine(url, pool_pre_ping=True, future=True)

    # --- payments ---

    def record_payment(
        self, saga_id: str, amount: float, status: str, ob: OutboxInsert
    ) -> bool:
        """Insert a payment row and its event in one transaction (FR-9).

        Idempotent on ``saga_id``: a duplicate order.created delivery does not
        double-charge. Returns True when the row was newly created.
        """
        with self._engine.begin() as conn:
            result = conn.execute(
                text(
                    """
                    INSERT INTO payments (id, saga_id, amount, status)
                    VALUES (gen_random_uuid(), :saga_id, :amount, :status)
                    ON CONFLICT (saga_id) DO NOTHING
                    RETURNING id
                    """
                ),
                {"saga_id": saga_id, "amount": amount, "status": status},
            )
            created = result.first() is not None
            if created:
                self._insert_outbox(conn, ob)
            return created

    def payment_status(self, saga_id: str) -> str | None:
        with self._engine.connect() as conn:
            row = conn.execute(
                text("SELECT status FROM payments WHERE saga_id = :saga_id"),
                {"saga_id": saga_id},
            ).first()
            return row[0] if row else None

    def refund(self, saga_id: str, idempotency_key: str, ob: OutboxInsert) -> bool:
        """Refund a processed payment idempotently (FR-3, FR-12, compensation).

        Returns False if the message was already processed (duplicate).
        """
        with self._engine.begin() as conn:
            if not self._mark_processed(conn, idempotency_key, REFUND_CONSUMER):
                return False
            updated = conn.execute(
                text(
                    """
                    UPDATE payments SET status = 'REFUNDED'
                    WHERE saga_id = :saga_id AND status = 'PROCESSED'
                    RETURNING id
                    """
                ),
                {"saga_id": saga_id},
            ).first()
            if updated is not None:
                self._insert_outbox(conn, ob)
            return True

    # --- idempotency ---

    @staticmethod
    def _mark_processed(conn, idempotency_key: str, consumer: str) -> bool:
        """Insert a dedupe marker; returns False if already processed (FR-12)."""
        result = conn.execute(
            text(
                """
                INSERT INTO processed_messages (idempotency_key, consumer)
                VALUES (:key, :consumer)
                ON CONFLICT DO NOTHING
                RETURNING idempotency_key
                """
            ),
            {"key": idempotency_key, "consumer": consumer},
        )
        return result.first() is not None

    # --- outbox (relay side, implements OutboxRepo) ---

    @staticmethod
    def _insert_outbox(conn, ob: OutboxInsert) -> None:
        conn.execute(
            text(
                """
                INSERT INTO outbox
                    (id, aggregate_type, aggregate_id, event_type, topic, payload, headers, status)
                VALUES
                    (gen_random_uuid(), :atype, :aid, :etype, :topic,
                     CAST(:payload AS JSONB),
                     jsonb_build_object('traceparent', CAST(:traceparent AS TEXT)),
                     'PENDING')
                """
            ),
            {
                "atype": ob.aggregate_type,
                "aid": ob.aggregate_id,
                "etype": ob.event_type,
                "topic": ob.topic,
                "payload": ob.payload.decode("utf-8"),
                "traceparent": ob.traceparent,
            },
        )

    def fetch_pending(self, limit: int) -> list[OutboxRecord]:
        with self._engine.begin() as conn:
            rows = conn.execute(
                text(
                    """
                    SELECT id, topic, aggregate_id, payload::text,
                           COALESCE(headers->>'traceparent', '')
                    FROM outbox
                    WHERE status = 'PENDING'
                    ORDER BY created_at
                    FOR UPDATE SKIP LOCKED
                    LIMIT :limit
                    """
                ),
                {"limit": limit},
            ).all()
        return [
            OutboxRecord(
                id=str(r[0]),
                topic=r[1],
                aggregate_id=r[2],
                payload=r[3].encode("utf-8"),
                traceparent=r[4],
            )
            for r in rows
        ]

    def mark_published(self, record_id: str) -> None:
        with self._engine.begin() as conn:
            conn.execute(
                text(
                    "UPDATE outbox SET status = 'PUBLISHED', published_at = now() "
                    "WHERE id = :id"
                ),
                {"id": record_id},
            )

    def mark_failed(self, record_id: str) -> None:
        with self._engine.begin() as conn:
            conn.execute(
                text("UPDATE outbox SET attempts = attempts + 1 WHERE id = :id"),
                {"id": record_id},
            )
