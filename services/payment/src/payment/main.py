"""Entry point for the Payment service (Python).

Wires observability, the payments repository, the Kafka consumer that drives the
payment saga step + compensation, the transactional-outbox relay, and the
synchronous Inventory gRPC client (ARCHITECTURE §3).
"""

from __future__ import annotations

import logging
import signal
import threading

from payment import config, obs
from payment.app import PaymentProcessor
from payment.db import Database
from payment.inventory_client import InventoryClient
from payment.kafka_io import KafkaConsumer, KafkaProducer
from payment.outbox import Relay

logger = logging.getLogger("payment.main")


def _relay_loop(relay: Relay, interval: float, stop: threading.Event) -> None:
    """Poll the outbox and publish PENDING rows until asked to stop (FR-10)."""
    while not stop.is_set():
        try:
            relay.drain_once()
        except Exception:  # noqa: BLE001 - relay must survive transient broker errors
            logger.exception("outbox relay drain failed")
        stop.wait(interval)


def main() -> int:
    cfg = config.load()
    tracer, meter = obs.setup(cfg.service_name, cfg.otlp_endpoint)
    logger.info("payment service starting brokers=%s db_configured=%s", cfg.kafka_brokers, True)

    db = Database(cfg.database_url)
    producer = KafkaProducer(cfg.kafka_brokers)
    inventory = InventoryClient(cfg.inventory_grpc_target)

    processor = PaymentProcessor(
        repo=db,
        reserver=inventory,
        tracer=tracer,
        meter=meter,
        force_failure=cfg.force_failure,
    )
    relay = Relay(db, producer, cfg.relay_batch_size)
    consumer = KafkaConsumer(
        cfg.kafka_brokers, cfg.consumer_group, PaymentProcessor.consumed_topics()
    )

    stop = threading.Event()

    def _shutdown(_signum: int, _frame: object) -> None:
        logger.info("shutdown signal received")
        consumer.stop()
        stop.set()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    relay_thread = threading.Thread(
        target=_relay_loop, args=(relay, cfg.relay_interval_seconds, stop), daemon=True
    )
    relay_thread.start()

    try:
        consumer.run(processor.handle)  # blocks until stop() is called
    finally:
        stop.set()
        relay_thread.join(timeout=5.0)
        inventory.close()
        logger.info("payment service stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
