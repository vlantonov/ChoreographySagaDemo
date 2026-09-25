"""Shared fakes for Payment unit tests (no live Kafka/Postgres, ARCHITECTURE §11)."""

from __future__ import annotations

from dataclasses import dataclass, field

from opentelemetry import metrics, trace

from payment.db import OutboxInsert
from payment.envelope import Envelope
from payment.inventory_client import ReserveResult, ReserveStatus

# Default global (no-op) OTel providers are used: create_counter and
# start_as_current_span work without any exporter configured.
TRACER = trace.get_tracer("payment-test")
METER = metrics.get_meter("payment-test")


@dataclass
class FakeRepo:
    """In-memory stand-in for ``db.Database`` implementing ``app.PaymentRepo``."""

    payments: dict[str, tuple[float, str]] = field(default_factory=dict)
    outbox: list[OutboxInsert] = field(default_factory=list)
    _refund_keys: set[str] = field(default_factory=set)

    def record_payment(self, saga_id: str, amount: float, status: str, ob: OutboxInsert) -> bool:
        if saga_id in self.payments:  # idempotent on saga_id (duplicate delivery)
            return False
        self.payments[saga_id] = (amount, status)
        self.outbox.append(ob)
        return True

    def refund(self, saga_id: str, idempotency_key: str, ob: OutboxInsert) -> bool:
        if idempotency_key in self._refund_keys:  # dedupe via processed_messages
            return False
        self._refund_keys.add(idempotency_key)
        cur = self.payments.get(saga_id)
        if cur is not None and cur[1] == "PROCESSED":
            self.payments[saga_id] = (cur[0], "REFUNDED")
            self.outbox.append(ob)
        return True


@dataclass
class FakeReserver:
    """In-memory stand-in for ``InventoryClient`` implementing ``app.Reserver``."""

    status: ReserveStatus = ReserveStatus.RESERVED
    calls: list[tuple[str, str, int, str]] = field(default_factory=list)

    def reserve_stock(
        self, saga_id: str, idempotency_key: str, sku: str, quantity: int, traceparent: str
    ) -> ReserveResult:
        self.calls.append((saga_id, sku, quantity, traceparent))
        return ReserveResult(status=self.status, reservation_id="res-1", message="")


def order_created(
    saga_id: str = "saga-1",
    amount: float = 10.0,
    sku: str = "SKU-1",
    quantity: int = 2,
) -> bytes:
    return Envelope(
        event_type="OrderCreated",
        saga_id=saga_id,
        data={
            "orderId": saga_id,
            "customerId": "cust-1",
            "sku": sku,
            "quantity": quantity,
            "amount": amount,
        },
    ).to_json()


def reservation_failed(saga_id: str = "saga-1", amount: float = 10.0) -> bytes:
    return Envelope(
        event_type="InventoryReservationFailed",
        saga_id=saga_id,
        data={"orderId": saga_id, "sagaId": saga_id, "amount": amount},
    ).to_json()
