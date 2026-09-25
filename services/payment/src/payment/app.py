"""Payment application layer: the saga step and its compensation.

This module wires the domain rule (``decide_payment``) to persistence (the
``PaymentRepo`` protocol), the synchronous Inventory gRPC leg (the ``Reserver``
protocol), and the transactional outbox. It performs no I/O of its own beyond
its injected collaborators, so it is unit-testable with in-memory fakes
(ARCHITECTURE §3, §6, §7, §8; FR-9, FR-12).
"""

from __future__ import annotations

import logging
from typing import Protocol

import grpc
from opentelemetry import metrics

from payment.db import OutboxInsert
from payment.domain import decide_payment
from payment.envelope import Envelope, parse
from payment.inventory_client import ReserveResult
from payment.obs import context_from_traceparent, inject_traceparent

logger = logging.getLogger("payment.app")

# Consumed topics (ARCHITECTURE §2, §9). Payment refunds solely off
# inventory.reservation_failed; order.cancelled is a terminal event it does not
# consume (ARCHITECTURE §9, §14 D2).
TOPIC_ORDER_CREATED = "order.created"
TOPIC_INVENTORY_RESV_FAILED = "inventory.reservation_failed"

# Produced topics (tech-stack §5.3).
TOPIC_PAYMENT_PROCESSED = "payment.processed"
TOPIC_PAYMENT_FAILED = "payment.failed"
TOPIC_PAYMENT_REFUNDED = "payment.refunded"

_AGGREGATE_TYPE = "payment"


class PaymentRepo(Protocol):
    """Persistence surface the processor needs (implemented by ``db.Database``)."""

    def record_payment(
        self, saga_id: str, amount: float, status: str, ob: OutboxInsert
    ) -> bool: ...

    def refund(self, saga_id: str, idempotency_key: str, ob: OutboxInsert) -> bool: ...


class Reserver(Protocol):
    """The synchronous Inventory gRPC leg (implemented by ``InventoryClient``)."""

    def reserve_stock(
        self, saga_id: str, idempotency_key: str, sku: str, quantity: int, traceparent: str
    ) -> ReserveResult: ...


class PaymentProcessor:
    """Handles ``order.created`` and the compensation events idempotently."""

    def __init__(
        self,
        repo: PaymentRepo,
        reserver: Reserver,
        tracer,
        meter: metrics.Meter,
        force_failure: bool,
    ) -> None:
        self._repo = repo
        self._reserver = reserver
        self._tracer = tracer
        self._force_failure = force_failure
        self._payment_result = meter.create_counter(
            "payment_result_total",
            description="payment outcomes by result (tech-stack §9.3)",
        )

    @staticmethod
    def consumed_topics() -> list[str]:
        return [TOPIC_ORDER_CREATED, TOPIC_INVENTORY_RESV_FAILED]

    def handle(self, topic: str, value: bytes, traceparent: str) -> None:
        """Kafka handler entry point (matches ``kafka_io.Handler``).

        Continues the distributed trace from the event's W3C context, then routes
        to the payment step or the compensation step. Raises on unexpected errors
        so the consumer redelivers (idempotency absorbs the duplicate).
        """
        env = parse(value)
        parent_ctx = context_from_traceparent(env.traceparent or traceparent)
        with self._tracer.start_as_current_span(f"consume {topic}", context=parent_ctx):
            out_traceparent = inject_traceparent()
            if topic == TOPIC_ORDER_CREATED:
                self._handle_order_created(env, out_traceparent)
            elif topic == TOPIC_INVENTORY_RESV_FAILED:
                self._handle_compensation(env, out_traceparent)
            else:
                logger.warning("ignoring unexpected topic %s", topic)

    def _handle_order_created(self, env: Envelope, traceparent: str) -> None:
        saga_id = env.saga_id
        amount = float(env.data.get("amount", 0.0))
        sku = str(env.data.get("sku", ""))
        quantity = int(env.data.get("quantity", 0))

        decision = decide_payment(amount, self._force_failure)
        if not decision.approved:
            ob = self._outbox(
                saga_id, "PaymentFailed", TOPIC_PAYMENT_FAILED,
                _failed_event(saga_id, amount, decision.reason, traceparent), traceparent,
            )
            if self._repo.record_payment(saga_id, amount, "FAILED", ob):
                self._payment_result.add(1, {"result": "failed"})
                logger.info("payment failed saga_id=%s reason=%s", saga_id, decision.reason)
            else:
                logger.info("duplicate order.created ignored saga_id=%s", saga_id)
            return

        ob = self._outbox(
            saga_id, "PaymentProcessed", TOPIC_PAYMENT_PROCESSED,
            _processed_event(saga_id, amount, traceparent), traceparent,
        )
        if not self._repo.record_payment(saga_id, amount, "PROCESSED", ob):
            logger.info("duplicate order.created ignored saga_id=%s", saga_id)
            return
        self._payment_result.add(1, {"result": "processed"})
        logger.info("payment processed saga_id=%s amount=%s", saga_id, amount)

        # Synchronous gRPC leg (tech-stack §6, ARCHITECTURE §7). Best-effort: a
        # transport error is logged rather than re-raised, because the payment is
        # already committed and its payment.processed event is queued — retrying
        # the consumer would be deduped and never re-issue this call.
        try:
            result = self._reserver.reserve_stock(
                saga_id, env.idempotency_key, sku, quantity, traceparent
            )
            logger.info("reserve_stock saga_id=%s status=%s", saga_id, result.status.name)
        except grpc.RpcError as exc:
            logger.error("reserve_stock transport error saga_id=%s: %s", saga_id, exc)

    def _handle_compensation(self, env: Envelope, traceparent: str) -> None:
        saga_id = env.saga_id
        amount = float(env.data.get("amount", 0.0))
        ob = self._outbox(
            saga_id, "PaymentRefunded", TOPIC_PAYMENT_REFUNDED,
            _refunded_event(saga_id, amount, traceparent), traceparent,
        )
        if not self._repo.refund(saga_id, env.idempotency_key, ob):
            logger.info("duplicate compensation ignored saga_id=%s", saga_id)
            return
        self._payment_result.add(1, {"result": "refunded"})
        logger.info("payment refunded saga_id=%s", saga_id)

    @staticmethod
    def _outbox(
        saga_id: str, event_type: str, topic: str, payload: bytes, traceparent: str
    ) -> OutboxInsert:
        return OutboxInsert(
            aggregate_type=_AGGREGATE_TYPE,
            aggregate_id=saga_id,
            event_type=event_type,
            topic=topic,
            payload=payload,
            traceparent=traceparent,
        )


def _processed_event(saga_id: str, amount: float, traceparent: str) -> bytes:
    return Envelope(
        event_type="PaymentProcessed",
        saga_id=saga_id,
        traceparent=traceparent,
        data={"orderId": saga_id, "sagaId": saga_id, "amount": amount, "status": "PROCESSED"},
    ).to_json()


def _failed_event(saga_id: str, amount: float, reason: str, traceparent: str) -> bytes:
    return Envelope(
        event_type="PaymentFailed",
        saga_id=saga_id,
        traceparent=traceparent,
        data={
            "orderId": saga_id,
            "sagaId": saga_id,
            "amount": amount,
            "reason": reason,
            "status": "FAILED",
        },
    ).to_json()


def _refunded_event(saga_id: str, amount: float, traceparent: str) -> bytes:
    return Envelope(
        event_type="PaymentRefunded",
        saga_id=saga_id,
        traceparent=traceparent,
        data={"orderId": saga_id, "sagaId": saga_id, "amount": amount, "status": "REFUNDED"},
    ).to_json()
