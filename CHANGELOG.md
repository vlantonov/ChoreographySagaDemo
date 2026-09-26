# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.4] — 2026-09-26

### Changed

- **Reproducible, faster inventory image build.** Pinned vcpkg to release tag `2026.07.29`
  (`git clone --branch`) with a matching `builtin-baseline`
  (`9e593bb18ea69cc5095e012465dcd675a822ed0d`) in `vcpkg.json`, removing the previous
  floating-`HEAD` clone that made the build non-reproducible. Added a release-only overlay
  triplet (`x64-linux-release`, stock `x64-linux` + `VCPKG_BUILD_TYPE release`) so
  dependencies compile once instead of in both debug and release, roughly halving the
  toolchain build time.

## [0.1.3] — 2026-09-26

### Fixed

- **Inventory image build (vcpkg stage).** The C++ Inventory Dockerfile build stage did not
  install the native build tools vcpkg needs for its port graph, so the image build failed.
  Added `python3` (required to build `vcpkg-tool-meson`), the GNU autotools packages
  (`autoconf`, `automake`, `libtool`, `autoconf-archive`) used by transitive grpc/protobuf
  ports, and `bison`/`flex` (required by the `libpq` port) to the build stage.
- **Inventory server compilation (OpenTelemetry C++ API).** First full server build surfaced
  two genuine compile errors in `obs_otel.cpp`: `TextMapPropagator::Extract` takes a non-const
  `Context&` and cannot bind the `RuntimeContext::GetCurrent()` temporary (now passed via a
  named lvalue), and `AddMetricReader` exists only on the SDK `MeterProvider` (now downcast
  from the API type returned by `MeterProviderFactory::Create`).

## [0.1.2] — 2026-09-26

### Fixed

- **Compensation sequence diagram render.** The final `Note` in the compensation-path
  Mermaid diagram used a semicolon, which Mermaid treats as a statement separator, breaking
  the rich diagram render on GitHub. Replaced it with a comma so the note is a single
  statement.

## [0.1.1] — 2026-09-26

### Fixed

- **Inventory (C++) CI build break from the outbox SLO-metrics change.** The new
  `outbox_pending` SLO gauge added `outbox::Repo::count_pending()` and the
  `obs::record_outbox_publish_lag` / `obs::set_outbox_lag_recorder` core hooks, but
  three implementations were left incomplete:
  - `testing::FakeOutboxRepo` (`test/fakes.hpp`) did not override `count_pending()`, so it
    stayed abstract and `relay_test.cpp` failed to compile — added an override returning the
    count of not-yet-published rows.
  - The core `obs.cpp` declared the `g_outbox_lag` recorder but never defined
    `record_outbox_publish_lag` / `set_outbox_lag_recorder`, breaking the `inventory_core`
    link (undefined reference from the relay) — added both definitions (no-op until a
    recorder is installed).
  - The production `db::Database` (`db.hpp` / `db.cpp`), which also derives from
    `outbox::Repo`, was missing `count_pending()` — added the override (`SELECT count(*)
    FROM outbox WHERE status = 'PENDING'`), matching the existing `fetch_pending` SQL. This
    would have broken the server build (`INVENTORY_BUILD_SERVER=ON`), which CI does not
    exercise.
  - Added `Relay.PendingCountTracksUnpublishedRows` regression test covering the backlog gauge.

## [0.1.0] — 2026-09-26

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

[0.1.4]: https://github.com/vladiant/ChoreographySagaDemo/releases/tag/v0.1.4
[0.1.3]: https://github.com/vladiant/ChoreographySagaDemo/releases/tag/v0.1.3
[0.1.2]: https://github.com/vladiant/ChoreographySagaDemo/releases/tag/v0.1.2
[0.1.1]: https://github.com/vladiant/ChoreographySagaDemo/releases/tag/v0.1.1
[0.1.0]: https://github.com/vladiant/ChoreographySagaDemo/releases/tag/v0.1.0
