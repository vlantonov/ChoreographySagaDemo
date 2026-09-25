"""Kafka producer/consumer wrappers (confluent-kafka) with manual offset commit
and W3C traceparent propagation via headers (tech-stack §3, FR-23)."""

from __future__ import annotations

import logging
from collections.abc import Callable

from confluent_kafka import Consumer, KafkaError, Producer

logger = logging.getLogger("payment.kafka")

TRACEPARENT_HEADER = "traceparent"


class KafkaProducer:
    """Implements the outbox ``Publisher`` protocol."""

    def __init__(self, brokers: str) -> None:
        self._producer = Producer({"bootstrap.servers": brokers, "enable.idempotence": True})

    def publish(self, topic: str, key: str, payload: bytes, traceparent: str) -> None:
        errors: list[str] = []

        def _cb(err, _msg):  # noqa: ANN001
            if err is not None:
                errors.append(str(err))

        headers = [(TRACEPARENT_HEADER, traceparent.encode("utf-8"))] if traceparent else None
        self._producer.produce(topic, key=key.encode("utf-8"), value=payload, headers=headers, callback=_cb)
        self._producer.flush()
        if errors:
            raise RuntimeError(f"kafka publish failed: {errors[0]}")


# Handler receives (topic, value_bytes, traceparent) and raises on failure.
Handler = Callable[[str, bytes, str], None]


class KafkaConsumer:
    def __init__(self, brokers: str, group: str, topics: list[str]) -> None:
        self._consumer = Consumer(
            {
                "bootstrap.servers": brokers,
                "group.id": group,
                "enable.auto.commit": False,
                "auto.offset.reset": "earliest",
            }
        )
        self._topics = topics
        self._running = False

    def run(self, handler: Handler) -> None:
        self._consumer.subscribe(self._topics)
        self._running = True
        try:
            while self._running:
                msg = self._consumer.poll(1.0)
                if msg is None:
                    continue
                if msg.error():
                    if msg.error().code() == KafkaError._PARTITION_EOF:
                        continue
                    logger.error("consume error: %s", msg.error())
                    continue
                traceparent = ""
                for k, v in msg.headers() or []:
                    if k == TRACEPARENT_HEADER and v is not None:
                        traceparent = v.decode("utf-8")
                try:
                    handler(msg.topic(), msg.value(), traceparent)
                except Exception:  # noqa: BLE001 - do not commit; redelivery + idempotency
                    logger.exception("handler failed; message will be redelivered")
                    continue
                self._consumer.commit(msg, asynchronous=False)
        finally:
            self._consumer.close()

    def stop(self) -> None:
        self._running = False
