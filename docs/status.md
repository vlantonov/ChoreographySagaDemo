# Project Status — Choreography Saga Demo

Project Manager running the SDLC agent chain for a new portfolio project.

## Pipeline

New-project chain: **requirements-analyst → system-architect → developer → qa-engineer → release-engineer → technical-writer**

| Stage | Owner | Status | Notes |
|-------|-------|--------|-------|
| Requirements | Requirements Analyst | ✅ Done | `docs/requirements/SRS.md` (FR-1…26, NFR-1…7, C-1…16, AC-1…13, OQ-1…9) |
| Design / Tech Stack | System Architect | ✅ Done | `docs/tech-stack.md`, `docs/design/ARCHITECTURE.md`, `docs/design/sequence-diagram.md`; OQ-1…9 resolved |
| Implementation | Developer | ✅ Done | 3 services built; Order(Go)/Payment(py,15 tests)/Inventory(C++,10 tests) pass. Deploy/observability configs deferred to Release. |
| QA / Verification | QA Engineer | ✅ Conditional-pass | Go 8 / Py 16 / C++ 10 tests pass; consistency verified. Defects D1–D4 logged; D1 routed to Architect. Regression after Release. |
| Release / Packaging | Release Engineer | ⬜ Not started (after D1 loop) | Compose, Helm, K8s, observability backends, CI |
| Documentation | Technical Writer | ⬜ Not started | |

## Key design decisions (from System Architect)
- Language mapping: **Order=Go 1.23, Payment=Python 3.12, Inventory=C++20**.
- Sync gRPC leg: `Inventory.ReserveStock` (called by Payment); `ReleaseStock` for compensation.
- SQL: PostgreSQL, database-per-service; polling outbox relay (`FOR UPDATE SKIP LOCKED`) + `processed_messages` idempotency table.
- Kafka topics: `order.created`, `payment.processed`, `payment.failed`, `inventory.reserved`, `inventory.reservation_failed`, `payment.refunded`, `order.cancelled`.
- Forced failure: amount `66.06` / `FORCE_PAYMENT_FAILURE=true` (payment stage); SKU `SKU-DEADBEEF` (inventory stage, full reverse chain).
- Order trigger: gRPC `CreateOrder` + REST via grpc-gateway; demo scripts.
- Observability: OTel → Collector → Prometheus/Loki/Tempo → Grafana; trace_id/span_id in structured logs.

## Open questions carried forward
- All OQ-1…9 resolved by the System Architect. Non-blocking note: compensation `ReleaseStock`/`InventoryReleased` captured consistent with FR-3 (no scope expansion).

## Commit log (per-stage, semver-classified)
- Requirements stage: `d1e726d` semver(minor).
- Design stage: `ae93770` semver(minor).
- Implementation stage: `20a8a1c` semver(minor).
- QA stage: pending commit.

## Open defects (QA)
- **D1 (Medium)** — out-of-order cross-topic delivery can strand saga in `PAYMENT_OK` (Order drops undefined transitions instead of retrying). Routed to System Architect for fix-vs-accept.
- **D2 (Low)** — Payment consumes `order.cancelled` (not in design); inflates refunded metric, state correct. Developer.
- **D3 (Low, doc)** — ARCHITECTURE §2/6/9 vs §7 disagree on Inventory consuming `payment.processed`; code follows sync-gRPC design. System Architect doc fix.
- **D4 (Info)** — unused `orders.payment_id`/`reservation_id` columns.

## Implementation notes / carry-forward for QA & Release
- Inventory (C++) full server target (grpc/libpqxx/opentelemetry-cpp adapters) needs a vcpkg toolchain or the Docker build to compile; only core+tests compiled locally. QA/Release should run the full vcpkg build.
- Deploy manifests (compose/helm/k8s), observability backend configs (OTel Collector, Prometheus + alert rules, Loki, Tempo, Grafana dashboards/SLOs), and CI are the Release Engineer's scope.
- `buf.gen.yaml` lists an optional C++ codegen path; CMake generates C++ stubs directly.
