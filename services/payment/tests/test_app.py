"""Application-layer tests for the Payment saga step and compensation.

All collaborators are in-memory fakes (helpers module); no Kafka or Postgres is
required (ARCHITECTURE §11).
"""

from __future__ import annotations

import json

from helpers import METER, TRACER, FakeRepo, FakeReserver, order_created, reservation_failed

from payment.app import (
    TOPIC_INVENTORY_RESV_FAILED,
    TOPIC_ORDER_CREATED,
    TOPIC_PAYMENT_FAILED,
    TOPIC_PAYMENT_PROCESSED,
    TOPIC_PAYMENT_REFUNDED,
    PaymentProcessor,
)
from payment.inventory_client import ReserveStatus


def _make(repo: FakeRepo, reserver: FakeReserver, force_failure: bool = False) -> PaymentProcessor:
    return PaymentProcessor(
        repo=repo,
        reserver=reserver,
        tracer=TRACER,
        meter=METER,
        force_failure=force_failure,
    )


def test_happy_path_records_payment_and_reserves_stock() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver)

    processor.handle(TOPIC_ORDER_CREATED, order_created(amount=10.0, sku="SKU-1", quantity=2), "")

    assert repo.payments["saga-1"] == (10.0, "PROCESSED")
    assert len(repo.outbox) == 1
    ob = repo.outbox[0]
    assert ob.topic == TOPIC_PAYMENT_PROCESSED
    assert ob.event_type == "PaymentProcessed"
    # The synchronous gRPC leg was invoked with the order's sku/quantity.
    assert reserver.calls == [("saga-1", "SKU-1", 2, "")]


def test_outbox_payload_is_a_valid_envelope() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    _make(repo, reserver).handle(TOPIC_ORDER_CREATED, order_created(amount=25.5), "")

    body = json.loads(repo.outbox[0].payload)
    assert body["eventType"] == "PaymentProcessed"
    assert body["sagaId"] == "saga-1"
    assert body["schemaVersion"] == 1
    assert body["data"]["amount"] == 25.5
    assert body["idempotencyKey"]  # each emitted event carries its own key


def test_forced_failure_via_flag_emits_payment_failed() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver, force_failure=True)

    processor.handle(TOPIC_ORDER_CREATED, order_created(amount=10.0), "")

    assert repo.payments["saga-1"] == (10.0, "FAILED")
    assert repo.outbox[0].topic == TOPIC_PAYMENT_FAILED
    assert reserver.calls == []  # no reservation attempted on failure


def test_forced_failure_via_magic_amount_emits_payment_failed() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver, force_failure=False)

    processor.handle(TOPIC_ORDER_CREATED, order_created(amount=66.06), "")

    assert repo.payments["saga-1"][1] == "FAILED"
    assert repo.outbox[0].topic == TOPIC_PAYMENT_FAILED
    assert reserver.calls == []


def test_duplicate_order_created_is_deduped() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver)
    event = order_created(amount=10.0)

    processor.handle(TOPIC_ORDER_CREATED, event, "")
    processor.handle(TOPIC_ORDER_CREATED, event, "")  # redelivery

    assert len(repo.payments) == 1
    assert len(repo.outbox) == 1  # no second PaymentProcessed
    assert len(reserver.calls) == 1  # gRPC leg not re-issued


def test_compensation_refunds_processed_payment() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver)
    repo.payments["saga-1"] = (10.0, "PROCESSED")

    processor.handle(TOPIC_INVENTORY_RESV_FAILED, reservation_failed(amount=10.0), "")

    assert repo.payments["saga-1"] == (10.0, "REFUNDED")
    assert repo.outbox[-1].topic == TOPIC_PAYMENT_REFUNDED
    assert repo.outbox[-1].event_type == "PaymentRefunded"


def test_duplicate_compensation_is_deduped() -> None:
    repo, reserver = FakeRepo(), FakeReserver()
    processor = _make(repo, reserver)
    repo.payments["saga-1"] = (10.0, "PROCESSED")
    event = reservation_failed(amount=10.0)

    processor.handle(TOPIC_INVENTORY_RESV_FAILED, event, "")
    processor.handle(TOPIC_INVENTORY_RESV_FAILED, event, "")  # same idempotency key

    refunds = [ob for ob in repo.outbox if ob.topic == TOPIC_PAYMENT_REFUNDED]
    assert len(refunds) == 1


def test_reserve_stock_transport_error_does_not_raise() -> None:
    import grpc

    class ExplodingReserver(FakeReserver):
        def reserve_stock(self, *args, **kwargs):
            raise grpc.RpcError("boom")

    repo = FakeRepo()
    processor = _make(repo, ExplodingReserver())

    # Must not raise: payment is committed and its event queued regardless.
    processor.handle(TOPIC_ORDER_CREATED, order_created(amount=10.0), "")
    assert repo.payments["saga-1"] == (10.0, "PROCESSED")


def test_insufficient_stock_leaves_payment_processed() -> None:
    repo = FakeRepo()
    reserver = FakeReserver(status=ReserveStatus.INSUFFICIENT_STOCK)
    processor = _make(repo, reserver)

    processor.handle(TOPIC_ORDER_CREATED, order_created(sku="SKU-DEADBEEF"), "")

    # Payment stays PROCESSED; Inventory emits reservation_failed and the refund
    # arrives later as a compensation event.
    assert repo.payments["saga-1"][1] == "PROCESSED"
    assert reserver.calls[0][1] == "SKU-DEADBEEF"
