# Software Requirements Specification (SRS)

## Choreography Saga Demo — Distributed Transaction Portfolio Project

| Field | Value |
| --- | --- |
| Project | ChoreographySagaDemo |
| Document type | Software Requirements Specification (SRS) |
| Version | 0.1 (Draft) |
| Author | Requirements Analyst |
| Status | Ready for System Architect review |
| Date | 2026-09-26 |

---

## 1. Overview

### 1.1 Purpose

This document specifies the requirements for a portfolio demonstration project that
implements a **distributed transaction across multiple services using the Choreography
Saga pattern** (i.e. no central orchestrator; services react to events emitted by peers).
The project exists primarily to demonstrate, in a resume/portfolio context, mastery of:

- Distributed data consistency without two-phase commit (Saga + compensating transactions).
- Reliable event publishing (Transactional Outbox).
- Idempotent message processing.
- A polyglot microservice stack (C++, Python, Go).
- Full-stack observability (metrics, logs, traces) with correlation and alerting.
- Modern packaging and deployment (Docker, Docker Compose, Helm, Kubernetes).

### 1.2 Business Context

A customer places an order. Fulfilling that order requires coordinating three independent
services that each own their own database: **Order**, **Payment**, and **Inventory**.
Because each service owns its data, a single ACID transaction cannot span all three. The
system must instead maintain eventual consistency via a saga, rolling back completed steps
through compensating actions when a later step fails.

### 1.3 Primary Use Case (Narrative)

1. A client submits a request to create an order.
2. The Order service persists the order in a pending state and publishes an
   `OrderCreated` event.
3. The Payment service reacts by attempting payment and publishes `PaymentProcessed`
   (or `PaymentFailed`).
4. The Inventory service reacts to successful payment by reserving stock and publishes
   `InventoryReserved` (or `InventoryReservationFailed`).
5. On success, the Order is marked confirmed. On failure at any step, compensating
   transactions run in reverse (release inventory, refund/void payment, cancel order).

### 1.4 Stakeholders

| Stakeholder | Interest |
| --- | --- |
| Project Owner (portfolio author) | Demonstrable, well-documented skills across the listed technologies. |
| Reviewers / Hiring Managers | Clear evidence of distributed-systems and observability competence. |
| System Architect (next stage) | A complete, testable requirement set to design against. |

---

## 2. Functional Requirements

Each requirement is independently testable and stated independently of implementation
language or framework.

### 2.1 Services & Saga Coordination

- **FR-1** The system shall consist of at least three cooperating services: **Order**,
  **Payment**, and **Inventory**, each owning its own persistent state.
- **FR-2** The services shall coordinate the distributed transaction using the
  **Choreography Saga pattern**, in which each service reacts to events emitted by other
  services. There shall be **no central orchestrator** component.
- **FR-3** Each saga step that mutates state shall define a corresponding **compensating
  transaction** that semantically undoes its effect (e.g. release reserved inventory,
  void/refund a processed payment, cancel a created order).

### 2.2 Order Workflow

- **FR-4** The Order service shall accept a request to create an order and persist it in a
  well-defined initial state (e.g. `PENDING`) before any event is published.
- **FR-5** Order creation shall trigger the saga flow in the sequence:
  order created → payment processed → inventory reserved.
- **FR-6** On successful completion of all steps, the order shall transition to a terminal
  success state (e.g. `CONFIRMED`).
- **FR-7** On failure of any step, the saga shall execute compensating transactions for all
  previously completed steps, in reverse order, and the order shall transition to a terminal
  failure state (e.g. `CANCELLED` / `FAILED`).
- **FR-8** Each service shall expose the current state of its portion of the saga in a way
  that can be queried or observed for verification (e.g. via a status endpoint, database
  record, and/or emitted event).

### 2.3 Reliable Messaging — Transactional Outbox

- **FR-9** Each service that publishes events shall use the **Transactional Outbox pattern**:
  the business state change and the outbound event record shall be written within the same
  local database transaction.
- **FR-10** A separate relay/publisher mechanism shall read pending outbox records and
  publish them to the event backbone, marking them as dispatched only after successful
  publication (at-least-once delivery).
- **FR-11** The outbox shall guarantee that no committed business state change is lost from
  the event stream, even if the publisher crashes and restarts.

### 2.4 Idempotency

- **FR-12** Every command/message handler shall support **idempotency keys** so that
  processing the same message more than once produces the same result and no duplicate side
  effects.
- **FR-13** Each service shall persist processed idempotency keys (or equivalent dedup
  state) durably so that duplicates are detected across restarts.

### 2.5 Persistence

- **FR-14** Each service shall persist its durable state (business entities, outbox,
  idempotency/dedup records, and saga step status) in a **SQL database dedicated to that
  service** (database-per-service; no shared schema across services).

### 2.6 Synchronous gRPC Contract

- **FR-15** At least one inter-service interaction shall be a **synchronous gRPC call**
  defined by a versioned **`.proto` contract** (e.g. a `ReserveInventory` RPC on the
  Inventory service).
- **FR-16** The `.proto` contract shall define request and response messages, including an
  outcome status suitable for driving saga success/compensation decisions.

### 2.7 Event Backbone — Kafka

- **FR-17** The asynchronous event flow between services shall use **Apache Kafka** as the
  event backbone, with services acting as producers and consumers.
- **FR-18** Saga events (e.g. `OrderCreated`, `PaymentProcessed`, `PaymentFailed`,
  `InventoryReserved`, `InventoryReservationFailed`, and compensation events) shall be
  published to and consumed from Kafka topics.

### 2.8 Forced-Failure / Compensation Scenario

- **FR-19** The system shall provide a **deliberately triggerable failure scenario**
  (e.g. an insufficient-inventory or forced-payment-failure condition) that causes a later
  saga step to fail.
- **FR-20** The forced failure shall cause the compensating transactions of earlier steps to
  execute, and this rollback shall be **observable via logs and distributed traces**.
- **FR-21** After compensation completes, all affected services shall return to a consistent
  state equivalent to the order never having been fulfilled (no reserved stock, no captured
  payment, order in a cancelled/failed terminal state).

### 2.9 Observability (Functional aspects)

- **FR-22** Each service shall emit **metrics, logs, and traces** for saga operations,
  including success and compensation paths.
- **FR-23** Traces shall propagate a correlation/trace context across service boundaries
  (both Kafka events and gRPC calls) so a single saga instance can be followed end to end.
- **FR-24** The system shall provide **Grafana dashboards** visualizing saga throughput,
  success vs. compensation outcomes, and relevant service-level indicators.
- **FR-25** The system shall define **alerting rules** (e.g. elevated compensation/failure
  rate) and at least one **SLO dashboard**.
- **FR-26** The system shall support **trace-to-log correlation** so that a trace can be
  navigated to its corresponding log entries.

---

## 3. Non-Functional Requirements

- **NFR-1 (Reliability / Consistency)** The system shall achieve eventual consistency across
  services such that, for every completed saga, the final states of all services are mutually
  consistent (fully committed or fully compensated) with no partial residue.
- **NFR-2 (Delivery semantics)** Event delivery shall be at-least-once; correctness under
  duplicate delivery shall be guaranteed by idempotency (see FR-12/FR-13).
- **NFR-3 (Durability)** Committed business state, outbox entries, and idempotency records
  shall survive service restarts and container/pod restarts.
- **NFR-4 (Observability coverage)** All three observability signals — metrics, logs, and
  traces — shall be available for every service and correlated for a given saga instance.
- **NFR-5 (Reproducibility)** The entire system shall be startable locally with a single
  command via Docker Compose, and deployable to Kubernetes via the provided artifacts,
  without manual per-service configuration steps beyond documented parameters.
- **NFR-6 (Documentation quality)** The repository shall contain documentation sufficient for
  a reviewer to understand and run the demo, including the required sequence diagram.
- **NFR-7 (Demonstrability)** The forced-failure scenario shall be runnable on demand and
  produce clearly observable evidence of compensation within the observability stack.

> Note: Quantitative targets (throughput, latency, exact SLO thresholds, resource limits)
> are intentionally left open (see Open Questions) as they are demo-scoped and to be set by
> the architect/owner.

---

## 4. Constraints (Stakeholder-Mandated)

These are explicit technology constraints supplied by the stakeholder. They are recorded as
binding constraints on the design; the specific version/toolchain selection within each
remains a System Architect decision.

### 4.1 Polyglot Implementation

- **C-1** The implementation shall be **polyglot**: at least one service shall be written in
  **C++**, at least one in **Python**, and at least one in **Go**.
- **C-2** The mapping of which language implements which service (Order/Payment/Inventory) is
  left to the System Architect, subject to C-1.

### 4.2 Observability Stack

- **C-3** Instrumentation shall use **OpenTelemetry**.
- **C-4** Metrics shall be collected via **Prometheus**.
- **C-5** Logs shall be aggregated via **Loki**.
- **C-6** Traces shall be collected via **Tempo**.
- **C-7** Dashboards, alerting, and SLO visualization shall be provided via **Grafana**.
- **C-8** The observability solution shall include **alerting rules**, at least one **SLO
  dashboard**, and **trace-to-log correlation**.

### 4.3 Messaging & RPC

- **C-9** The event backbone shall be **Apache Kafka**.
- **C-10** At least one synchronous interaction shall use **gRPC** with a **`.proto`**
  contract.

### 4.4 Persistence

- **C-11** Each service shall persist state in a **SQL database** (specific engine to be
  selected by the architect), using the database-per-service model.

### 4.5 Packaging & Deployment

- **C-12** The project shall provide a **multi-stage Dockerfile per service**.
- **C-13** The project shall provide a **Docker Compose** setup for local end-to-end runs.
- **C-14** The project shall provide a **Helm chart** for deployment.
- **C-15** The project shall provide raw **Kubernetes manifests**.

### 4.6 Documentation Deliverables

- **C-16** The project shall include a **sequence diagram** that covers **both the happy path
  and the compensation (rollback) path**.

---

## 5. Assumptions

- **A-1** This is a demonstration/portfolio system, not a production commercial system; scale,
  security hardening, and multi-tenancy are minimal unless later specified.
- **A-2** A single order/payment/inventory domain (one product line or simplified catalog) is
  sufficient to demonstrate the pattern; a rich domain model is not required.
- **A-3** Infrastructure dependencies (Kafka, SQL databases, the observability stack) run as
  containers alongside the services in both local and Kubernetes deployments.
- **A-4** "Payment" and "Inventory" are simulated business operations; no integration with
  real external payment processors or warehouse systems is expected.
- **A-5** Network and message delivery may fail or duplicate; the design must tolerate this
  (hence outbox + idempotency), but Byzantine faults are out of scope.

---

## 6. Out of Scope

- **OOS-1** Central orchestration (an orchestrator-based saga) — explicitly excluded by FR-2.
- **OOS-2** Real payment gateway / real inventory/warehouse integrations.
- **OOS-3** End-user UI/front end (a demo trigger such as an API call or script is sufficient).
- **OOS-4** Authentication/authorization, secrets management hardening, and production-grade
  security posture (may be noted as future work).
- **OOS-5** Multi-region / high-availability topology and capacity/load testing beyond what is
  needed to demonstrate the pattern.
- **OOS-6** Schema registry / formal event-schema governance (unless the architect chooses to
  include it).
- **OOS-7** CI/CD pipeline definition (may be added later; not required by this SRS).

---

## 7. Acceptance Criteria

### 7.1 Happy Path

- **AC-1** Given a valid order request, when the order is created, then payment is processed
  and inventory is reserved, and the order reaches the `CONFIRMED` (success) terminal state.
- **AC-2** A single distributed trace shall span Order → Payment → Inventory for one happy-path
  order and be viewable in Grafana/Tempo.
- **AC-3** Metrics in Grafana/Prometheus shall reflect the successful saga completion.

### 7.2 Forced-Failure / Compensation Path

- **AC-4** Given the forced-failure condition is enabled, when an order is created, then a
  later saga step fails and compensating transactions execute for all previously completed
  steps in reverse order.
- **AC-5** After compensation, the order shall be in a terminal `CANCELLED`/`FAILED` state,
  no inventory shall remain reserved, and no payment shall remain captured (fully compensated,
  per NFR-1).
- **AC-6** The compensation flow shall be observable end to end in logs **and** in a single
  distributed trace, with trace-to-log correlation demonstrable (per FR-26/C-8).
- **AC-7** An alert and/or SLO/dashboard signal shall reflect the elevated compensation/failure
  outcome (per FR-25).

### 7.3 Reliability Mechanisms

- **AC-8** With the outbox in use, killing a service immediately after committing a business
  state change (but before publish) shall not lose the corresponding event: it shall be
  published after restart (per FR-9–FR-11).
- **AC-9** Redelivering the same message (same idempotency key) shall not produce duplicate
  side effects; the resulting state shall be identical to single delivery (per FR-12/FR-13).

### 7.4 Packaging & Deployment

- **AC-10** `docker compose up` (or equivalent single command) shall bring up all services and
  infrastructure and allow a full happy-path and forced-failure run locally (per NFR-5, C-13).
- **AC-11** The Helm chart and Kubernetes manifests shall deploy the full system to a
  Kubernetes cluster (per C-14/C-15).
- **AC-12** Each service shall build from its multi-stage Dockerfile (per C-12).

### 7.5 Documentation

- **AC-13** The repository shall contain a sequence diagram covering both the happy path and
  the compensation path (per C-16).

---

## 8. Open Questions (for Stakeholder / System Architect)

- **OQ-1** Which language implements which service (Order/Payment/Inventory) given the C++,
  Python, Go constraint? (Architect decision, but stakeholder may have a preference.)
- **OQ-2** Which specific SQL engine(s) per service? Single engine for all, or intentionally
  heterogeneous to show breadth?
- **OQ-3** What quantitative SLO targets and alert thresholds are expected (e.g. saga success
  rate, p95 latency, compensation-rate alert threshold)?
- **OQ-4** Should there be a schema/versioning strategy for Kafka event payloads (e.g. schema
  registry), or are ad-hoc versioned schemas acceptable for the demo?
- **OQ-5** What is the exact forced-failure trigger mechanism — configuration flag, special
  input value (e.g. a "poison" SKU/amount), or a fault-injection endpoint?
- **OQ-6** Is a minimal trigger interface (REST endpoint, CLI, or script) preferred to start
  orders, given no UI is in scope?
- **OQ-7** Expected scale for the demo (single-instance per service vs. multiple replicas /
  partitioned Kafka consumers)? This affects idempotency and ordering considerations.
- **OQ-8** Is `docs/tech-stack.md` required as a next deliverable? This project is new and has
  no tech-stack document yet; the System Architect will need to create one to record concrete
  language/framework/engine choices.
- **OQ-9** Any licensing constraints on chosen infrastructure/libraries beyond the project's
  MIT license?

---

## 9. Glossary

| Term | Definition |
| --- | --- |
| **Saga** | A sequence of local transactions across multiple services, where each local transaction updates one service's data and triggers the next; consistency is maintained without a distributed ACID transaction. |
| **Choreography Saga** | A saga variant with no central coordinator; each service listens for events from others and decides autonomously what to do next, including when to compensate. |
| **Orchestration Saga** | (Contrast) A saga variant with a central orchestrator that tells each service what to do. Explicitly **not** used here (see FR-2/OOS-1). |
| **Compensating Transaction** | An action that semantically undoes the effect of a previously committed local transaction (e.g. release reserved inventory, refund a payment) when a later saga step fails. |
| **Transactional Outbox** | A pattern where a service writes its business state change and the outbound event to the same database in one local transaction; a separate relay later publishes the event, guaranteeing no committed change is lost. |
| **Outbox Relay / Message Relay** | The component that reads unpublished outbox rows and publishes them to the message broker, marking them dispatched on success. |
| **Idempotency** | The property that processing the same message/command multiple times yields the same result with no duplicate side effects. |
| **Idempotency Key** | A unique identifier attached to a message/command used to detect and discard duplicate processing. |
| **At-least-once delivery** | A messaging guarantee where a message is delivered one or more times (never lost, but possibly duplicated), which is why idempotency is required. |
| **Database-per-service** | An architectural rule where each service owns and exclusively accesses its own database, preventing shared-schema coupling. |
| **Event Backbone** | The messaging infrastructure (here, Kafka) over which services publish and consume domain events. |
| **gRPC** | A synchronous RPC framework using a `.proto` contract to define strongly-typed service methods and messages. |
| **OpenTelemetry (OTel)** | A vendor-neutral standard/SDK for generating and exporting metrics, logs, and traces. |
| **Trace-to-log correlation** | The ability to navigate from a distributed trace to the specific log entries emitted during that trace (and vice versa), typically via shared trace/span IDs. |
| **SLO** | Service Level Objective — a target value or range for a service-level indicator (e.g. saga success rate). |

---

## 10. Requirement Index (Summary)

- **Functional:** FR-1 … FR-26
- **Non-Functional:** NFR-1 … NFR-7
- **Constraints:** C-1 … C-16
- **Assumptions:** A-1 … A-5
- **Out of Scope:** OOS-1 … OOS-7
- **Acceptance Criteria:** AC-1 … AC-13
- **Open Questions:** OQ-1 … OQ-9
