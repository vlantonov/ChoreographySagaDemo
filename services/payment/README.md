# Payment Service (Python)

Payment is the Python 3.12 participant in the Choreography Saga demo. It consumes
`order.created`, applies the forced-failure business rule, makes the single
synchronous gRPC leg of the saga (`Inventory.ReserveStock`), and drives payment
compensation — all through a transactional outbox and an idempotent consumer
(tech-stack §1, ARCHITECTURE §2, §6, §7, §8).

## Responsibilities

| Concern | Detail |
| --- | --- |
| Consumes | `order.created`, `inventory.reservation_failed` |
| Publishes | `payment.processed`, `payment.failed`, `payment.refunded` |
| Sync API | gRPC **client** → `Inventory.ReserveStock` |
| Owns | `payments`, `outbox`, `processed_messages` (payments-db) |

### Saga flow

1. `order.created` → decide payment (forced-failure trigger: amount `66.06` or
   `FORCE_PAYMENT_FAILURE=true`).
   - Approved → persist `payments(PROCESSED)` + outbox `PaymentProcessed` in one
     transaction, then make the synchronous `ReserveStock` gRPC call.
   - Declined → persist `payments(FAILED)` + outbox `PaymentFailed`.
2. `inventory.reservation_failed` → refund a processed
   payment: `payments(REFUNDED)` + outbox `PaymentRefunded`.

Idempotency: the payment write is deduped on `saga_id`; compensation is deduped
via the `processed_messages` table (tech-stack §8.3, FR-12, FR-13).

## Layout

```text
services/payment/
├── pyproject.toml           # hatchling build, uv-managed deps (tech-stack §13)
├── Dockerfile               # multi-stage (C-12)
├── migrations/              # plain SQL applied by the Release Engineer's migrate job
├── src/payment/
│   ├── app.py               # application layer: saga step + compensation (pure, injectable)
│   ├── main.py              # runner: wires consumer, relay thread, gRPC client, OTel
│   ├── __main__.py          # `python -m payment`
│   ├── config.py db.py domain.py envelope.py
│   ├── inventory_client.py kafka_io.py obs.py outbox.py
│   └── pb/inventory/v1/     # generated gRPC stubs (see below)
└── tests/                   # pytest unit tests (fakes only, no live Kafka/Postgres)
```

## Build & test

```bash
# from services/payment
python3 -m venv .venv && . .venv/bin/activate
pip install -e '.[dev]'        # or: uv pip install -e '.[dev]'

# generate the Inventory gRPC stubs (see scripts/gen-proto.sh for the buf path)
python -m grpc_tools.protoc -I ../../proto \
  --python_out=src/payment/pb --grpc_python_out=src/payment/pb \
  ../../proto/inventory/v1/inventory.proto

pytest                         # unit tests
```

The repo-level `scripts/gen-proto.sh` regenerates stubs for all three services
via `buf generate` (preferred) or a `protoc` fallback and rewrites the Python
imports to the `payment.pb.inventory.v1` package.

## Configuration

| Env var | Default | Purpose |
| --- | --- | --- |
| `PAYMENT_DATABASE_URL` | `postgresql+psycopg://payment:payment@payments-db:5432/payments` | payments-db |
| `KAFKA_BROKERS` | `kafka:9092` | Kafka bootstrap |
| `KAFKA_CONSUMER_GROUP` | `payment-service` | consumer group |
| `INVENTORY_GRPC_TARGET` | `inventory:50052` | Inventory gRPC endpoint |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `http://otel-collector:4317` | OTLP/gRPC collector |
| `OTEL_SERVICE_NAME` | `payment` | service name in traces/metrics/logs |
| `FORCE_PAYMENT_FAILURE` | _(unset)_ | force every payment to fail (tech-stack §10) |
