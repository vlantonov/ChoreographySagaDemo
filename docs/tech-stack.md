# Tech Stack Decision Record

## Choreography Saga Demo

| Field | Value |
| --- | --- |
| Project | ChoreographySagaDemo |
| Document type | Tech-Stack Decision Record (authoritative) |
| Author | System Architect |
| Status | Approved for Developer hand-off |
| Date | 2026-09-26 |
| Source SRS | `docs/requirements/SRS.md` (v0.1) |

> This document is the single source of truth for concrete technology choices. It resolves
> Open Questions **OQ-1 … OQ-9** from the SRS and records the rationale/trade-off for every
> decision that had more than one reasonable option. Requirement IDs in parentheses map each
> decision back to the SRS.

---

## 0. Open-Question Resolution Summary

| OQ | Question | Resolution | Section |
| --- | --- | --- | --- |
| OQ-1 | Language-to-service mapping | Order=**Go**, Payment=**Python**, Inventory=**C++** | §1 |
| OQ-2 | SQL engine per service | **PostgreSQL 16** for all three, database-per-service | §4 |
| OQ-3 | SLO targets & alert thresholds | Defined quantitatively | §9 |
| OQ-4 | Kafka event schema/versioning | **JSON** envelope with `schemaVersion`, CloudEvents-style; no registry | §5 |
| OQ-5 | Forced-failure trigger | Magic order amount **`66.06`** OR `FORCE_PAYMENT_FAILURE=true` env flag | §10 |
| OQ-6 | Order-trigger interface | **gRPC + REST (grpc-gateway)** `CreateOrder` on Order service | §11 |
| OQ-7 | Demo scale | **Single replica per service**, single-partition topics (ordering-safe) | §7 |
| OQ-8 | Is `docs/tech-stack.md` required | **Yes** — this document | — |
| OQ-9 | Licensing constraints | All chosen components are permissively licensed (Apache-2.0/MIT/BSD/PostgreSQL) — compatible with project MIT | §13 |

---

## 1. Language-to-Service Mapping (OQ-1, C-1, C-2, FR-1)

| Service | Language | Rationale |
| --- | --- | --- |
| **Order** | **Go 1.23** | Order is the saga entry point and API front door. Go's first-class gRPC support, goroutine-based outbox relay, and `grpc-gateway` for REST make it the strongest fit for the coordination/entrypoint role. |
| **Payment** | **Python 3.12** | Payment hosts the forced-failure logic and benefits from Python's readability for demonstrating business/compensation rules. Mature `confluent-kafka` and `grpcio` bindings. |
| **Inventory** | **C++20** | Inventory exposes the **synchronous gRPC `ReserveStock` RPC** (§6). C++ demonstrates systems-language depth and pairs naturally with the performance-sensitive synchronous call. |

**Constraint satisfied:** each of C++, Python, Go is used by exactly one service (C-1).

**Trade-off considered:** an all-Go stack would be simplest to build but violates the mandated
polyglot constraint (C-1) and undersells the portfolio goal (§1.1 of SRS). The chosen mapping
places the hardest language (C++) on the service with the smallest surface area (Inventory:
one gRPC method + one compensation consumer), keeping build cost proportional to value.

---

## 2. gRPC Implementation Per Language (C-10, FR-15, FR-16)

| Language | gRPC stack | Codegen |
| --- | --- | --- |
| Go | `google.golang.org/grpc` + `protoc-gen-go` / `protoc-gen-go-grpc`; REST via `grpc-gateway` v2 | `buf generate` |
| Python | `grpcio` + `grpcio-tools` | `buf generate` (or `python -m grpc_tools.protoc`) |
| C++ | `grpc++` (gRPC C++ core) + `protoc` C++ plugin | CMake `protobuf_generate` / `buf generate` |

Proto toolchain: **`buf`** for linting, breaking-change detection, and multi-language codegen
from a single `proto/` module. Protobuf syntax: **proto3**.

**Trade-off:** raw `protoc` per language works but `buf` gives one lint/breaking-change gate
across all three languages and centralizes codegen config — worth the one extra tool.

---

## 3. Kafka Client Per Language (C-9, FR-17, FR-18)

| Language | Kafka client | Notes |
| --- | --- | --- |
| Go | `github.com/twmb/franz-go` | Pure-Go, no CGo; simple producer/consumer + manual offset commit for at-least-once. |
| Python | `confluent-kafka-python` | librdkafka-backed, high performance, manual commit. |
| C++ | `librdkafka` (C++ `RdKafka` API) | Same librdkafka core as Python; idiomatic C++ wrapper. |

All consumers use **manual offset commit after successful processing** to honor at-least-once
semantics (NFR-2) and pair with idempotency (§8).

---

## 4. SQL Engine & Topology (OQ-2, C-11, FR-14)

- **Engine:** **PostgreSQL 16** for all three services.
- **Topology:** database-per-service — three logical databases, no shared schema, no
  cross-service queries (FR-14). In Compose these are three separate Postgres containers
  (`orders-db`, `payments-db`, `inventory-db`); in Kubernetes, three StatefulSets.

| Language | SQL access | Migrations |
| --- | --- | --- |
| Go (Order) | `pgx/v5` + `sqlc` (typed queries, no heavyweight ORM) | `golang-migrate` |
| Python (Payment) | `SQLAlchemy 2.0` Core + `psycopg 3` | `Alembic` |
| C++ (Inventory) | `libpqxx` (official C++ PostgreSQL client) | plain SQL files applied by a small migrator step |

**Trade-off (OQ-2):** heterogeneous engines (e.g. add MySQL) would show breadth but multiply
operational surface and dashboards for no design benefit. A **single engine (PostgreSQL)**
keeps the observability/backup story uniform while still demonstrating database-per-service.
The breadth signal comes from the polyglot **drivers** (pgx / SQLAlchemy / libpqxx) instead.

---

## 5. Event Schema, Topics & Versioning (OQ-4, FR-17, FR-18)

### 5.1 Envelope format

**JSON** with a stable envelope (CloudEvents-inspired, no external registry — see OOS-6).

```json
{
  "eventId": "uuid",
  "eventType": "OrderCreated",
  "schemaVersion": 1,
  "occurredAt": "RFC3339 timestamp",
  "sagaId": "uuid (== orderId)",
  "idempotencyKey": "uuid",
  "traceparent": "W3C traceparent header for context propagation",
  "data": { /* event-type-specific payload */ }
}
```

**Trade-off (OQ-4):** Avro/Protobuf + Schema Registry gives stronger governance but adds a
Confluent Schema Registry dependency explicitly out of scope (OOS-6). **JSON + an explicit
`schemaVersion` integer** is sufficient for a demo, human-readable in Kafka UIs/logs, and
keeps the polyglot serialization trivial. Protobuf is still used where it earns its keep — the
synchronous gRPC contract (§6).

### 5.2 Versioning strategy

- Additive changes (new optional field) → same `schemaVersion`.
- Breaking changes → increment `schemaVersion`; consumers switch on it. `buf` guards the
  gRPC side; a documented rule guards the JSON side.

### 5.3 Topics (single partition each — see §7)

| Topic | Producer | Consumers | Purpose |
| --- | --- | --- | --- |
| `order.created` | Order | Payment | Forward saga step 1 (FR-5) |
| `payment.processed` | Payment | Inventory, Order | Payment success (FR-5) |
| `payment.failed` | Payment | Order | Payment failure → compensation trigger (FR-7) |
| `inventory.reserved` | Inventory | Order | Inventory success → order CONFIRMED (FR-6) |
| `inventory.reservation_failed` | Inventory | Order, Payment | Inventory failure → compensation (FR-7) |
| `payment.refunded` | Payment | Order | Compensation: payment voided/refunded (FR-3, FR-7) |
| `order.cancelled` | Order | (terminal) | Compensation: order cancelled (FR-7, FR-21) |

> Compensation events (`payment.refunded`, `order.cancelled`) and
> `inventory.reservation_failed` implement the reverse-order rollback (FR-3, FR-7, AC-4/AC-5).

---

## 6. Synchronous gRPC Contract Choice (OQ-4 partial, C-10, FR-15, FR-16)

- **Synchronous call:** `Inventory.ReserveStock` (Payment → Inventory, gRPC).
- **Why this one:** stock reservation is the step where the caller needs an **immediate,
  strongly-typed success/failure outcome** to decide whether to continue the saga or trigger
  compensation (FR-16). A request/response gRPC call models this decision point far better than
  fire-and-forget events, and it satisfies "at least one synchronous gRPC interaction" (C-10)
  on the service (Inventory=C++) that most benefits from a typed contract.
- Everything else in the saga remains **asynchronous over Kafka** (choreography, FR-2) — the
  gRPC call is one leg, not an orchestrator.

### `proto/inventory/v1/inventory.proto` (shape only — Developer implements)

```proto
syntax = "proto3";
package inventory.v1;

service InventoryService {
  rpc ReserveStock(ReserveStockRequest) returns (ReserveStockResponse);
  rpc ReleaseStock(ReleaseStockRequest) returns (ReleaseStockResponse); // compensation
}

message ReserveStockRequest {
  string saga_id = 1;
  string idempotency_key = 2;
  string sku = 3;
  int32  quantity = 4;
}

message ReserveStockResponse {
  enum Status { STATUS_UNSPECIFIED = 0; RESERVED = 1; INSUFFICIENT_STOCK = 2; }
  Status status = 1;
  string reservation_id = 2;
  string message = 3;
}

message ReleaseStockRequest  { string saga_id = 1; string reservation_id = 2; string idempotency_key = 3; }
message ReleaseStockResponse { bool released = 1; }
```

### `proto/order/v1/order.proto` (order-trigger interface — §11)

```proto
syntax = "proto3";
package order.v1;
import "google/api/annotations.proto"; // grpc-gateway REST mapping

service OrderService {
  rpc CreateOrder(CreateOrderRequest) returns (CreateOrderResponse) {
    option (google.api.http) = { post: "/v1/orders" body: "*" };
  }
  rpc GetOrder(GetOrderRequest) returns (GetOrderResponse) {
    option (google.api.http) = { get: "/v1/orders/{order_id}" };
  }
}

message CreateOrderRequest  { string sku = 1; int32 quantity = 2; double amount = 3; string customer_id = 4; }
message CreateOrderResponse { string order_id = 1; string status = 2; }
message GetOrderRequest     { string order_id = 1; }
message GetOrderResponse    { string order_id = 1; string status = 2; string sku = 3; int32 quantity = 4; double amount = 5; }
```

---

## 7. Demo Scale & Ordering (OQ-7, NFR-2)

- **One replica per service**, **one partition per topic**. Single partition guarantees
  per-topic ordering without a partition-key strategy, which keeps the choreography easy to
  reason about for the demo.
- Idempotency (§8) is still implemented and demonstrated (AC-9) so the design remains correct
  if scaled out later; the doc notes multi-partition/multi-replica as future work (OOS-5).

**Trade-off:** multi-partition + consumer groups would demonstrate scale but complicate
ordering and compensation reasoning for reviewers. Single-partition is the right demo default;
correctness under duplication is still proven via idempotency tests.

---

## 8. Outbox & Idempotency (FR-9…FR-13, NFR-2, NFR-3)

### 8.1 Outbox table (each publishing service — Order, Payment, Inventory)

| Column | Type | Notes |
| --- | --- | --- |
| `id` | UUID PK | outbox record id |
| `aggregate_type` | TEXT | e.g. `order`, `payment`, `inventory` |
| `aggregate_id` | TEXT | business entity id (== saga id) |
| `event_type` | TEXT | e.g. `OrderCreated` |
| `topic` | TEXT | target Kafka topic |
| `payload` | JSONB | full event envelope (§5.1) |
| `headers` | JSONB | includes `traceparent` for trace propagation (FR-23) |
| `created_at` | TIMESTAMPTZ | default `now()` |
| `published_at` | TIMESTAMPTZ NULL | set when relay confirms publish |
| `status` | TEXT | `PENDING` / `PUBLISHED` |
| `attempts` | INT | retry counter |

Business write + outbox insert occur in **one local DB transaction** (FR-9).

### 8.2 Outbox relay/publisher mechanism (FR-10, FR-11, AC-8)

- **Polling publisher** per service: a background loop (Go goroutine / Python asyncio task /
  C++ worker thread) selects `status='PENDING'` rows `ORDER BY created_at`, publishes to Kafka,
  and on broker ack sets `status='PUBLISHED', published_at=now()`.
- Uses `SELECT ... FOR UPDATE SKIP LOCKED` to be restart-safe and avoid double-dispatch.
- Because publish-then-mark can crash between steps, delivery is **at-least-once** (NFR-2);
  consumer-side idempotency (§8.3) absorbs duplicates. Satisfies AC-8 (event survives a crash
  after commit, published after restart).

**Trade-off:** polling vs. Debezium/logical-decoding CDC. CDC (Debezium) avoids polling lag but
adds a Kafka Connect + Debezium dependency and per-language complexity. **Polling** is simpler,
fully in-process, language-idiomatic, and adequate at demo scale; documented as the deliberate
choice with CDC noted as future work.

### 8.3 Idempotency store (each consuming service) (FR-12, FR-13, AC-9)

Table `processed_messages`:

| Column | Type | Notes |
| --- | --- | --- |
| `idempotency_key` | UUID / TEXT PK | from event envelope or gRPC request |
| `consumer` | TEXT | handler name (part of composite key) |
| `result_hash` | TEXT | optional: stored outcome for identical replay response |
| `processed_at` | TIMESTAMPTZ | default `now()` |

**Dedupe strategy:** handler opens a transaction, attempts `INSERT` of
`(idempotency_key, consumer)`; on unique-violation it treats the message as already processed,
commits its business effect **and** the marker atomically, then commits offset. This makes
reprocessing side-effect-free (FR-12) and durable across restarts (FR-13, NFR-3).

---

## 9. Observability Stack & SLOs (OQ-3, C-3…C-8, FR-22…FR-26)

### 9.1 Wiring

| Concern | Tool | Per-language SDK / mechanism |
| --- | --- | --- |
| Instrumentation | **OpenTelemetry** (C-3) | Go: `go.opentelemetry.io/otel`; Python: `opentelemetry-sdk` + auto-instrumentation for grpc/kafka/sqlalchemy; C++: `opentelemetry-cpp` |
| Export protocol | **OTLP/gRPC** to a single **OpenTelemetry Collector** | All services export traces + metrics + logs to the Collector |
| Metrics | **Prometheus** (C-4) | Collector exposes a Prometheus endpoint; Prometheus **scrapes the Collector** (and optional per-service `/metrics`) |
| Logs | **Loki** (C-5) | Structured JSON logs → Collector/promtail → Loki; every log line carries `trace_id`/`span_id` |
| Traces | **Tempo** (C-6) | Collector exports OTLP traces → Tempo |
| Dashboards/Alerts/SLO | **Grafana** (C-7, C-8) | Provisioned datasources (Prometheus, Loki, Tempo) + dashboards + alert rules as code |

### 9.2 Trace-to-log correlation (FR-26, C-8, AC-6)

- Every service logs **structured JSON** including `trace_id` and `span_id` from the active
  OTel span.
- Grafana **Tempo → Loki** derived field links `trace_id`, enabling one-click trace-to-logs.
- Kafka events carry `traceparent` in `headers` (§8.1) and gRPC uses the OTel propagator, so a
  single `sagaId` is followable end-to-end across async + sync hops (FR-23, AC-2, AC-6).

### 9.3 Concrete SLO targets & alert thresholds (OQ-3, FR-25, AC-7)

| SLI | SLO target | Alert threshold | Backing metric |
| --- | --- | --- | --- |
| Saga success rate | ≥ 99% of sagas reach a terminal state (CONFIRMED or cleanly CANCELLED) | page if terminalization < 95% over 5 min | `saga_terminal_total{outcome}` |
| Happy-path completion latency (create→CONFIRMED) | p95 ≤ 2s (demo-scale) | warn if p95 > 4s over 5 min | `saga_duration_seconds` histogram |
| Compensation rate | ≤ 5% of orders under normal operation | **warn if > 20% over 5 min** (the forced-failure demo intentionally trips this — AC-7) | `saga_terminal_total{outcome="cancelled"}` / total |
| Payment failure rate | ≤ 5% | warn if > 20% over 5 min | `payment_result_total{result="failed"}` |
| Outbox publish lag | p95 ≤ 1s from insert to `PUBLISHED` | warn if backlog > 100 pending or lag > 10s | `outbox_pending` gauge, `outbox_publish_lag_seconds` |
| gRPC ReserveStock latency | p95 ≤ 250ms | warn if p95 > 750ms | OTel RPC server histogram |

At least one dedicated **SLO dashboard** in Grafana renders success-rate and compensation-rate
against these targets (C-8, FR-25, AC-7).

---

## 10. Forced-Failure Trigger (OQ-5, FR-19, FR-20, AC-4)

Two equivalent triggers, both implemented in the **Payment** service:

1. **Magic amount:** an order `amount == 66.06` forces `PaymentFailed` (deterministic,
   requires no restart — ideal for live demos).
2. **Env flag:** `FORCE_PAYMENT_FAILURE=true` forces *every* payment to fail (useful for
   automated compensation tests / AC-4).

On a forced failure, Payment publishes `payment.failed`; Order runs compensation. If the
failure is configured to occur *after* payment (to also exercise inventory compensation), the
alternate trigger is a **poison SKU `SKU-DEADBEEF`** that makes `ReserveStock` return
`INSUFFICIENT_STOCK`, exercising the full reverse chain (release stock → refund → cancel).

**Trade-off:** a generic fault-injection endpoint is more flexible but adds a surface and code
path unrelated to the domain. Deterministic magic values + one env flag are the simplest
reviewer-runnable triggers (NFR-7).

---

## 11. Order-Trigger Interface (OQ-6, OOS-3, FR-4)

- Primary: **gRPC `OrderService.CreateOrder`** on the Order service (Go).
- Convenience: **REST** `POST /v1/orders` exposed via **grpc-gateway** over the same handler,
  so reviewers can trigger a saga with `curl` (no UI needed — OOS-3).
- A small **`scripts/run-demo.sh`** (and `scripts/force-failure.sh`) wraps the calls for the
  happy-path and compensation demos (NFR-7, AC-1, AC-4).

---

## 12. Infra Component Versions (NFR-5, C-13, C-14, C-15)

| Component | Version (pinned) | Image / source |
| --- | --- | --- |
| Apache Kafka | **3.8.x (KRaft mode, no ZooKeeper)** | `bitnami/kafka:3.8` or `apache/kafka:3.8.0` |
| PostgreSQL | **16.x** | `postgres:16-alpine` |
| OpenTelemetry Collector | **0.108.x (contrib)** | `otel/opentelemetry-collector-contrib:0.108.0` |
| Prometheus | **2.54.x** | `prom/prometheus:v2.54.1` |
| Loki | **3.1.x** | `grafana/loki:3.1.1` |
| Tempo | **2.6.x** | `grafana/tempo:2.6.0` |
| Grafana | **11.2.x** | `grafana/grafana:11.2.0` |
| Kafka UI (optional) | latest | `provectuslabs/kafka-ui` (dev convenience) |

> Versions are pinned for reproducibility (NFR-5); exact patch tags finalized by the Release
> Engineer.

---

## 13. Build, Dependency & Test Tooling Per Language (NFR-5, C-12)

| Concern | Go (Order) | Python (Payment) | C++ (Inventory) |
| --- | --- | --- | --- |
| Build system | `go build` / Go modules | `hatchling` build backend | **CMake ≥ 3.25** |
| Dependency manager | **Go modules** (`go.mod`) | **`uv`** (with `pyproject.toml`) | **vcpkg** (manifest mode `vcpkg.json`) |
| Test framework | `go test` + `testify` | `pytest` | **GoogleTest** |
| Lint/static analysis | `golangci-lint` | `ruff` + `mypy` | `clang-tidy` + `clang-format` |
| Container | multi-stage Dockerfile (C-12) | multi-stage Dockerfile (C-12) | multi-stage Dockerfile (C-12) |

**Trade-offs:**
- Python deps: `uv` chosen over bare `pip`/Poetry for fast, reproducible, lockfile-based installs
  that fit multi-stage Docker builds well.
- C++ deps: `vcpkg` (manifest mode) chosen over Conan for its simpler CMake toolchain
  integration and broad coverage of gRPC/protobuf/librdkafka/libpqxx/opentelemetry-cpp. Conan
  is a valid alternative; vcpkg keeps the toolchain file one-line for CMake.

---

## 14. Licensing (OQ-9)

All selected components are permissively licensed and compatible with the project's MIT
license: Go/gRPC (BSD/Apache-2.0), Python/grpcio (Apache-2.0), gRPC C++/librdkafka/libpqxx
(Apache-2.0/BSD), PostgreSQL (PostgreSQL License), Kafka/Prometheus/Loki/Tempo/Grafana OSS/OTel
(Apache-2.0/AGPL-for-Grafana-OSS-server-use is acceptable for self-hosted demo use). No
copyleft obligation affects the project's own source. **No blocking licensing constraints.**

---

## 15. Requirement Coverage Map

| SRS ID | Addressed by |
| --- | --- |
| FR-1, FR-2, FR-3 | §1, §5.3 (topics incl. compensation) |
| FR-4…FR-8 | §5.3, §6, §11, ARCHITECTURE saga/data model |
| FR-9…FR-11 | §8.1, §8.2 |
| FR-12, FR-13 | §8.3 |
| FR-14 | §4 |
| FR-15, FR-16 | §6 |
| FR-17, FR-18 | §3, §5 |
| FR-19, FR-20, FR-21 | §10, §5.3 |
| FR-22…FR-26 | §9 |
| NFR-1…NFR-7 | §7, §8, §9, §12 |
| C-1…C-16 | §1–§3, §4, §6, §9, §12, §13 |
| OQ-1…OQ-9 | §0 summary + referenced sections |
