# Project Status — Choreography Saga Demo

Project Manager running the SDLC agent chain for a new portfolio project.

## Pipeline

New-project chain: **requirements-analyst → system-architect → developer → qa-engineer → release-engineer → technical-writer**

| Stage | Owner | Status | Notes |
|-------|-------|--------|-------|
| Requirements | Requirements Analyst | ✅ Done | `docs/requirements/SRS.md` (FR-1…26, NFR-1…7, C-1…16, AC-1…13, OQ-1…9) |
| Design / Tech Stack | System Architect | ✅ Done | `docs/tech-stack.md`, `docs/design/ARCHITECTURE.md`, `docs/design/sequence-diagram.md`; OQ-1…9 resolved |
| Implementation | Developer | ✅ Done | 3 services built; Order(Go)/Payment(py,15 tests)/Inventory(C++,10 tests) pass. Deploy/observability configs deferred to Release. |
| QA / Verification | QA Engineer | ✅ Conditional-pass | Go/Py/C++ suites pass; D1/D2 fixed & re-verified; D3 doc fixed; D4 accepted. Live regression after Release. |
| Release / Packaging | Release Engineer | ✅ Done (metrics loop next) | Compose+Helm+K8s, OTel/Prom/Loki/Tempo/Grafana, alerts, SLO+trace-log dashboards, CI. Env footgun fixed; latency-SLO metrics gap flagged. |
| Documentation | Technical Writer | ✅ Done | Top-level `README.md` (portfolio overview, both sequence diagrams, quickstart, forced-failure demo, observability, deploy/build/test) + `CHANGELOG.md` (Keep a Changelog, `[Unreleased]` for PM to finalize). Doc drift flagged (see below). |

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
- QA stage: `2c69aa3` semver(patch).
- D1/D2/D3 remediation loop (Architect+Developer): `9af2d12` semver(patch).
- Release stage (packaging + observability + CI): `c5720d4` semver(minor).
- SLO metrics loop (Developer metrics + Release alerts/panels): `91c9c1a` semver(minor).
- Documentation stage (README/CHANGELOG + demo scripts + go.mod fix): `896b7e2` semver(minor).
- Version publish: v0.1.0 (initial release, MINOR).
- Maintenance: fix C++ Inventory CI build (Maintenance Engineer) → v0.1.1 (PATCH).

## Post-release maintenance
- **CI fix (C++ Inventory):** the outbox SLO-metrics change left three interface members
  unimplemented — `FakeOutboxRepo::count_pending` (broke `relay_test.cpp`), the core
  `obs.cpp` lag-recorder definitions (broke `inventory_core` link), and a real production
  `db::Database::count_pending` gap (server build, not exercised by CI). All fixed; a
  `Relay.PendingCountTracksUnpublishedRows` regression test added. CI-equivalent core+tests
  build passes 11/11. The `db.cpp` production fix still needs the full vcpkg/Docker server
  build to compile-verify.

## Documentation-stage doc drift (resolved)
- Demo scripts `scripts/run-demo.sh` and `scripts/force-failure.sh` now shipped (target the
  Order REST gateway on :8080; drive happy-path and both forced-failure/compensation scenarios).
- `services/order/go.mod` raised to `go 1.23` to match tech-stack/CI; README updated.

## Carry-forward (latency-SLO gap)
- RESOLVED: services now emit `saga_duration_seconds`, `outbox_pending`, `outbox_publish_lag_seconds` (Order/Payment/Inventory) and `reserve_stock_server_duration_seconds` (Inventory); latency/lag alerts + dashboard panels added. C++ metric code verifies on the vcpkg/Docker build.
- Post-Release live regression still owed: full compose bring-up, forced-failure demo tripping compensation + latency alerts, Grafana trace↔log click-through, C++ full vcpkg/Docker server build, helm/promtool/otelcol/kubeconform on a tooled CI box.

## Open defects (QA)
- **D1 (Medium)** — FIXED: Order now parks premature out-of-order events (`ErrPrematureEvent`) and the consumer rewinds/redelivers instead of dropping. New domain tests + live-Kafka reorder check deferred to post-Release regression.
- **D2 (Low)** — FIXED: Payment refunds only on `inventory.reservation_failed`; `order.cancelled` consumer removed.
- **D3 (Low, doc)** — FIXED: ARCHITECTURE sections aligned to the sync-gRPC design.
- **D4 (Info)** — ACCEPTED: `orders.payment_id`/`reservation_id` kept as reserved (documented).

## Implementation notes / carry-forward for QA & Release
- Inventory (C++) full server target (grpc/libpqxx/opentelemetry-cpp adapters) needs a vcpkg toolchain or the Docker build to compile; only core+tests compiled locally. QA/Release should run the full vcpkg build.
- Deploy manifests (compose/helm/k8s), observability backend configs (OTel Collector, Prometheus + alert rules, Loki, Tempo, Grafana dashboards/SLOs), and CI are the Release Engineer's scope.
- `buf.gen.yaml` lists an optional C++ codegen path; CMake generates C++ stubs directly.
