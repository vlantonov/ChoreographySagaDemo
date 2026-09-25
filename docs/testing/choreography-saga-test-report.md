# QA Test Report — Choreography Saga Demo

| Field | Value |
| --- | --- |
| Project | ChoreographySagaDemo |
| Stage | QA / Verification (code-level / component) |
| Owner | QA Engineer |
| Date | 2026-09-26 |
| Sources | `docs/requirements/SRS.md`, `docs/design/ARCHITECTURE.md`, `docs/tech-stack.md` |
| Verdict | **CONDITIONAL-PASS** |

> **Scope of this pass:** code-level / component verification only. The Docker Compose
> stack, Helm, K8s, and observability backends (OTel Collector, Prometheus, Loki, Tempo,
> Grafana) do **not** exist yet — they are the Release Engineer's stage. Full live
> end-to-end (real Kafka/Postgres/Grafana), trace-to-log correlation, SLO dashboards and
> alerts are **out of scope now** and are deferred to a post-Release regression pass.

---

## 1. Test Results Per Service

Commands taken from `docs/tech-stack.md` §13.

### Order (Go 1.23) — PASS

- `go build ./...` → **success**.
- `go test ./...` → **8/8 tests PASS**.

| Test | Package | Verifies |
| --- | --- | --- |
| `TestApply_HappyPath` | domain | PENDING→PAYMENT_OK→CONFIRMED (AC-1) |
| `TestApply_PaymentFailureCancels` | domain | payment-stage failure → CANCELLED (AC-4) |
| `TestApply_InventoryFailureCompensationChain` | domain | reverse-order compensation → CANCELLED (AC-4/AC-5) |
| `TestApply_IdempotentDuplicatesAndTerminals` | domain | duplicate/terminal no-ops (AC-9) |
| `TestApply_InvalidTransition` | domain | undefined transition rejected |
| `TestEnvelopeRoundTrip` | event | JSON envelope round-trip |
| `TestRelay_DrainPublishesAndMarks` | outbox | relay publishes + marks PUBLISHED (AC-8) |
| `TestRelay_PublishFailureKeepsRowPending` | outbox | publish failure leaves row PENDING (AC-8) |

_Adapters (`cmd`, `app`, `config`, `grpcapi`, `kafkax`, `obs`, `store`, `pb`) have no unit tests — logic sits behind interfaces and is covered via the domain/outbox tests; DB/Kafka paths need the integration stack._

### Payment (Python 3.12) — PASS

- `python -m venv` + `pip install -e '.[dev]'` → **success**.
- `pytest` → **16/16 tests PASS** (15 pre-existing + **1 QA-added**, see §4).

Covers: happy path + sync gRPC leg, forced failure via magic amount `66.06` and via
`FORCE_PAYMENT_FAILURE`, `order.created` dedup (AC-9), refund compensation (AC-4/AC-5),
compensation dedup (AC-9), gRPC transport-error tolerance, `INSUFFICIENT_STOCK` handling,
and (new) `order.cancelled` compensation no-op guard.

### Inventory (C++20) — PASS (core+tests) / server build DEFERRED

- `cmake -S . -B build -DINVENTORY_BUILD_SERVER=OFF -DINVENTORY_BUILD_TESTS=ON` → **success**
  (nlohmann_json + GoogleTest resolved via FetchContent fallback — no vcpkg present).
- `cmake --build` + `ctest` → **10/10 tests PASS**: envelope round-trip/tolerant-parse,
  relay drain/failure (AC-8), reserve success decrements stock (AC-1), poison-SKU +
  insufficient-stock failure (AC-4), release restores stock (AC-5), repeated reserve/release
  dedup (AC-9).
- **The full `inventory_server` target (gRPC / librdkafka / libpqxx / opentelemetry-cpp) was
  NOT compiled** — those dependencies require the vcpkg toolchain or the Docker build. The
  gRPC server, Kafka I/O, Postgres repository, and OTel adapter were verified by **code
  inspection only**. Release/CI must run the full vcpkg (or Docker) build.

---

## 2. Acceptance-Criteria Coverage Table

Legend: **(a)** covered by an automated test · **(b)** verifiable by code inspection ·
**(c)** blocked pending the Release compose/observability stack.

| AC | Summary | Status | Evidence |
| --- | --- | --- | --- |
| AC-1 | Happy path → `CONFIRMED` | (a) component + (c) e2e | `TestApply_HappyPath`; Payment `test_happy_path…`; Inventory `ReserveSuccessDecrementsStock`. Full 3-service flow needs live Kafka. |
| AC-2 | Single trace Order→Payment→Inventory in Tempo | (b) + (c) | `traceparent` in envelope, Kafka header, gRPC metadata; propagator extract/inject on consume+produce in all 3 services. Grafana/Tempo view is Release scope. |
| AC-3 | Metrics reflect success in Prometheus | (b) + (c) | `saga_terminal_total`, `payment_result_total`, reserve metric emitted. Prometheus/Grafana is Release scope. |
| AC-4 | Forced failure → reverse-order compensation | (a) component + (c) e2e | Order domain compensation-chain test; Payment forced-failure tests; Inventory `PoisonSkuReservationFails`. |
| AC-5 | Post-compensation: CANCELLED, no reserved stock, no captured payment | (a) component + (c) e2e | Inventory `ReleaseRestoresStock`; Payment refund test; Order chain. No-residue e2e needs live stack. |
| AC-6 | Compensation observable in logs **and** one trace; trace-to-log correlation | (b) + (c) | Structured logging + `trace_id`/traceparent propagation present. Tempo→Loki derived-field correlation is Release scope. |
| AC-7 | Alert / SLO signal for elevated compensation | (c) | Backing metrics emitted; **alert rules + SLO dashboard do not exist yet** (Release scope). |
| AC-8 | Outbox survives crash, published after restart | (a) component + (c) e2e | Relay tests (Go/Python/C++) + `FOR UPDATE SKIP LOCKED` in all 3 migrations/queries. Kill-after-commit e2e needs live stack. |
| AC-9 | Redelivery → no duplicate side effects | (a) | Order `TestApply_Idempotent…` + `processed_messages`; Payment dedup tests; Inventory `RepeatedReserve/ReleaseIsDeduped`. |
| AC-10 | `docker compose up` full local run | (c) | **Compose file does not exist** — Release scope. |
| AC-11 | Helm chart + K8s manifests deploy | (c) | **Do not exist** — Release scope. |
| AC-12 | Each service builds from multi-stage Dockerfile | (b) + (c) | Dockerfiles present for all 3; Go/Python builds verified locally; Inventory Docker (vcpkg) build not executed. |
| AC-13 | Sequence diagram: happy + compensation | **(b) PASS** | `docs/design/sequence-diagram.md` contains both the happy path and the poison-SKU/payment-failure compensation paths. |

**Key FR spot-checks:** FR-1/FR-2 (3 services, no orchestrator) ✔ code; FR-3/FR-7
(compensation reverse order) ✔ domain tests; FR-4/FR-6 (state machine) ✔; FR-9 (business
write + outbox in one tx) ✔ code (all 3); FR-10/FR-11 (relay, SKIP LOCKED) ✔ tests;
FR-12/FR-13 (idempotency `processed_messages`) ✔ tests; FR-14 (db-per-service) ✔ migrations;
FR-15/FR-16 (sync gRPC `ReserveStock` typed outcome) ✔ proto+client+server; FR-17/FR-18
(Kafka topics) ✔; FR-19 (forced failure) ✔; FR-22/FR-23 (OTel + trace propagation) ✔ code.

---

## 3. Cross-Service Consistency Checks — ALL PASS

| Check | Result | Notes |
| --- | --- | --- |
| Kafka topic names match across producers/consumers + design | ✅ | Identical constants: Order `app/service.go`, Payment `app.py`, Inventory `service.hpp`, design tables, sequence diagram. All 7 topics agree. |
| Event-envelope JSON field names | ✅ | `eventId, eventType, schemaVersion, occurredAt, sagaId, idempotencyKey, traceparent, data` identical in Go `event.Envelope`, Python `Envelope.to_json`, C++ `make_envelope`; `schemaVersion=1` everywhere. |
| `traceparent` propagation | ✅ | Kafka header key `traceparent` (all 3), gRPC metadata `traceparent` (Payment client + Inventory server), envelope field carried through outbox `headers` JSONB; propagator extract on consume, inject on produce. |
| Outbox schema across 3 migrations | ✅ | Same columns (`id, aggregate_type, aggregate_id, event_type, topic, payload JSONB, headers JSONB, status, attempts, created_at, published_at`) + partial PENDING index; relay uses `FOR UPDATE SKIP LOCKED` in all three. |
| `processed_messages` idempotency schema | ✅ | Same columns + `PRIMARY KEY (idempotency_key, consumer)` across Order/Payment/Inventory migrations. |
| Sync gRPC contract (proto ↔ client ↔ server) | ✅ | `ReserveStock`/`ReleaseStock` field numbers + `Status{RESERVED=1, INSUFFICIENT_STOCK=2}` match Payment `inventory_client.py` and Inventory `grpc_server.cpp`. |
| Forced-failure triggers wired | ✅ | Amount `66.06` (`domain.MAGIC_FAILURE_AMOUNT`), `FORCE_PAYMENT_FAILURE` env (`force_failure_from_env`), poison SKU `SKU-DEADBEEF` (`domain.hpp kPoisonSku` + seeded zero-stock row). All have tests. |

---

## 4. Test-Only Additions Made This Pass

- **Payment** `tests/test_app.py::test_order_cancelled_without_processed_payment_emits_no_refund`
  (+ `order_cancelled` helper in `tests/helpers.py`). Closes a coverage gap: the
  `order.cancelled` consumed topic had **zero** test coverage, and the defensive
  `status='PROCESSED'` refund guard was untested. The test asserts that a compensation
  trigger for a non-processed (FAILED) payment emits **no** `PaymentRefunded` event and does
  not mutate payment state. Suite: 15 → **16 passing**. No production code changed.

---

## 5. Defect List

| ID | Sev | Title | Owner (stage) |
| --- | --- | --- | --- |
| D1 | **Medium** | Cross-topic out-of-order delivery can strand the saga | System Architect / Developer |
| D2 | Low | Payment consumes `order.cancelled` → inflated `refunded` metric + misleading log | Developer |
| D3 | Low (doc) | ARCHITECTURE internally inconsistent on reservation mechanism | System Architect |
| D4 | Info | `orders.payment_id` / `orders.reservation_id` columns never populated | Developer (optional) |

### D1 — Out-of-order delivery can strand the saga (Medium)

`app.Service.HandleEvent` (Order) treats any undefined state transition as a poison message:
`domain.Apply` returns `ErrInvalidTransition`, the handler logs `"invalid transition;
skipping"` and returns `nil`, which **commits the Kafka offset and drops the event**.
`payment.processed` and `inventory.reserved` are separate topics with **no mutual ordering
guarantee**, and Inventory's outbox relay is an independent poller from Payment's. If
`inventory.reserved` reaches Order before `payment.processed`, Order is still `PENDING`,
`Apply(PENDING, InventoryReserved)` is undefined → the event is discarded. The order then
advances to `PAYMENT_OK` on the later `payment.processed` but **never reaches `CONFIRMED`** —
violating AC-1 under reordering.

- **Impact:** latent correctness bug; may not surface in a calm single-replica demo but is
  not guaranteed safe. Also applies to other legitimate-but-early events.
- **Recommendation (for Architect/Developer):** do not silently drop plausibly-early events —
  return an error to force redelivery (park/retry) until the state can accept them, or
  reconcile against current state, rather than committing the offset. Requires a design
  decision (accept-for-demo vs. fix).
- **Verification:** cannot be reproduced without live Kafka → **must be exercised in the
  post-Release regression** (induce reordering / relay-lag).

### D2 — Payment `order.cancelled` compensation is a spurious no-op (Low)

Payment's consumed topics include `order.cancelled` (`app.py`), which is **not** in
ARCHITECTURE §2's Payment consume list (`order.created`, `inventory.reservation_failed`).
On a payment-stage failure, `order.cancelled` routes to `_handle_compensation`, which
increments `payment_result_total{result="refunded"}` and logs `"payment refunded"` even
though the `status='PROCESSED'` SQL guard correctly suppresses any actual refund/event.
No state corruption or duplicate event — only an inflated metric and a misleading log.
Now documented by the new test (§4). Fix: align consumed topics with the design, or skip the
metric/log when no `PROCESSED` payment was updated.

### D3 — ARCHITECTURE reservation-mechanism inconsistency (Low, doc)

`ARCHITECTURE.md` §2 (table), §6 (step 3), and §9 (topic matrix) state Inventory **consumes
`payment.processed`** to "drive ReserveStock", but §7 and `tech-stack.md` §6 state **Payment
makes the synchronous gRPC `ReserveStock` call**. The implementation follows §7 (Payment →
gRPC; Inventory does **not** consume `payment.processed`). This is a **documentation** defect,
not a code defect — the code matches the authoritative sync-gRPC design. The design doc should
remove the "Inventory consumes `payment.processed`" rows to avoid misleading future readers.

### D4 — Unused order columns (Informational)

`orders.payment_id` / `orders.reservation_id` are defined but never written by the Order
store. FR-8 is still satisfied via `orders.status`. Optional: populate them for a richer
`GetOrder`, or drop them.

---

## 6. Verdict & Handoff

**Verdict: CONDITIONAL-PASS (code-level stage).** All existing and QA-added unit suites are
green across the polyglot stack (Order 8, Payment 16, Inventory 10). Topics, event envelope,
`traceparent` propagation, outbox + `processed_messages` schemas, the synchronous gRPC
contract, and all forced-failure triggers are consistent across the three services and the
design. The one Medium defect (D1) is a latent robustness gap that requires the live stack to
confirm and does not block packaging.

**Handoff to the Project Manager:**

- ✅ **The work can proceed to the Release Engineer.** Component quality is sufficient to build
  the Compose/Helm/K8s + observability stage. Defect **D1 (Medium)** must be logged and routed
  to the **System Architect / Developer** for a decision (fix now vs. accept-for-demo);
  **D3** is a doc fix for the **System Architect**; **D2/D4** are Low/optional for the
  **Developer**. None block Release.
- 🔁 **Post-Release regression (QA, after Release builds infra) must cover:**
  1. AC-1/AC-4/AC-5 full 3-service happy-path and forced-failure runs on live Kafka/Postgres,
     including no-residue state checks.
  2. **D1** specifically — induce cross-topic reordering / outbox-relay lag and confirm the
     saga still reaches a terminal state (or fix D1 first).
  3. AC-2/AC-6 single distributed trace end-to-end and trace-to-log correlation in
     Grafana/Tempo→Loki.
  4. AC-3/AC-7 metrics, SLO dashboard, and the compensation-rate alert.
  5. AC-8 kill-after-commit-before-publish durability against live Kafka.
  6. AC-10/AC-11/AC-12 `docker compose up`, Helm/K8s deploy, and the Inventory multi-stage
     Docker (vcpkg) build + full `inventory_server` target compile.

_No commits made — the Project Manager coordinates commits._
