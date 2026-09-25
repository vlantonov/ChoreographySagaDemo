"""Payment service configuration from environment variables.

Defaults match the hostnames/ports in docs/tech-stack.md so the service runs
unmodified inside the stack owned by the Release Engineer.
"""

from __future__ import annotations

import os
from dataclasses import dataclass

from payment.domain import force_failure_from_env


@dataclass(frozen=True)
class Config:
    database_url: str
    kafka_brokers: str
    consumer_group: str
    inventory_grpc_target: str
    otlp_endpoint: str
    service_name: str
    force_failure: bool
    relay_interval_seconds: float
    relay_batch_size: int


def load() -> Config:
    return Config(
        database_url=os.environ.get(
            "PAYMENT_DATABASE_URL",
            "postgresql+psycopg://payment:payment@payments-db:5432/payments",
        ),
        kafka_brokers=os.environ.get("KAFKA_BROKERS", "kafka:9092"),
        consumer_group=os.environ.get("KAFKA_CONSUMER_GROUP", "payment-service"),
        inventory_grpc_target=os.environ.get("INVENTORY_GRPC_TARGET", "inventory:50052"),
        otlp_endpoint=os.environ.get("OTEL_EXPORTER_OTLP_ENDPOINT", "http://otel-collector:4317"),
        service_name=os.environ.get("OTEL_SERVICE_NAME", "payment"),
        force_failure=force_failure_from_env(),
        relay_interval_seconds=float(os.environ.get("OUTBOX_RELAY_INTERVAL", "0.5")),
        relay_batch_size=int(os.environ.get("OUTBOX_RELAY_BATCH", "100")),
    )
