# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] — initial release

Initial, feature-complete release of the Choreography Saga demo: a polyglot
distributed-transaction showcase implementing the saga pattern with compensating
transactions across three services and databases.

### Added

- **Three polyglot services**, each owning a private PostgreSQL database
  (database-per-service):
  - **Order** (Go) — saga entry point; inbound gRPC `CreateOrder` / `GetOrder` with a
    REST gateway (grpc-gateway); owns saga state.
  - **Payment** (Python) — consumes `order.created`, applies the forced-failure rule,
    makes the synchronous gRPC leg, and drives payment compensation.
  - **Inventory** (C++) — exposes the synchronous gRPC `ReserveStock` / `ReleaseStock`;
    owns stock and reservations.
- **Choreography saga** with no central orchestrator: the Order state machine
  (`PENDING → PAYMENT_OK → CONFIRMED`, with `COMPENSATING → CANCELLED`) is driven purely
  by consumed events, and **reverse-order compensating transactions** (release stock →
  refund payment → cancel order) roll back partial failures.
- **Transactional outbox** in every publishing service: business write + outbox insert in
  one local transaction, relayed by a polling publisher using `FOR UPDATE SKIP LOCKED`
  (at-least-once, crash-safe).
- **Consumer-side idempotency** via a `processed_messages` table, making redelivery
  side-effect-free.
- **gRPC contracts** (`proto/order/v1`, `proto/inventory/v1`) with reproducible multi-language
  codegen through `buf` (protoc fallback) in [scripts/gen-proto.sh](scripts/gen-proto.sh).
- **Kafka event backbone** with a stable JSON envelope (`schemaVersion`, `sagaId`,
  `idempotencyKey`, `traceparent`) over single-partition topics: `order.created`,
  `payment.processed`, `payment.failed`, `inventory.reserved`, `inventory.reservation_failed`,
  `payment.refunded`, `order.cancelled`, `inventory.released`.
- **Forced-failure triggers** for the compensation demo: magic amount `66.06` and the
  `FORCE_PAYMENT_FAILURE` env flag (payment stage), and poison SKU `SKU-DEADBEEF`
  (inventory stage, full reverse chain).
- **Three-signal observability stack:** OpenTelemetry → Collector → Prometheus (metrics),
  Tempo (traces), Loki (logs via Promtail); Grafana with provisioned datasources, a
  **Saga SLO Overview** dashboard, a **Trace & Log Correlation** dashboard, and
  Tempo↔Loki trace-to-log correlation. Prometheus alert rules cover compensation rate,
  payment-failure rate, saga success rate, inventory-failure rate, and latency/lag SLIs.
- **Packaging:** one-command Docker Compose stack (Kafka, PostgreSQL-per-service, migrations,
  services, observability), a Helm chart, and raw Kubernetes manifests (Helm/K8s deploy the
  three services against external Kafka/Postgres/OTLP endpoints); multi-stage Dockerfiles for
  each service.
- **CI** ([.github/workflows/ci.yml](.github/workflows/ci.yml)): per-language build+test,
  multi-stage image builds, and config validation (compose config, helm lint/template,
  kubeconform, promtool, otelcol validate).

### Fixed

- **D1 — out-of-order event handling:** the Order saga consumer now parks legitimately
  premature (cross-topic out-of-order) events and rewinds/redelivers them instead of
  dropping, preserving saga correctness under single-partition per-topic ordering.
- **D2 — refund trigger:** Payment now refunds only on `inventory.reservation_failed`; the
  redundant `order.cancelled` refund path was removed so a payment is not refunded twice.

[Unreleased]: https://github.com/vladiant/ChoreographySagaDemo
