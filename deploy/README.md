# Deploying the Choreography Saga Demo

> Minimal operator notes. The Technical Writer owns the full project README/CHANGELOG.

## One-command local stack (Docker Compose)

Brings up Kafka (KRaft), one PostgreSQL per service (migrations auto-applied),
the three services (built from their multi-stage Dockerfiles), and the full
observability backbone (OTel Collector, Prometheus, Loki, Tempo, Grafana,
Promtail):

```bash
docker compose -f deploy/compose/docker-compose.yml up --build
```

Endpoints:

| What | URL |
| --- | --- |
| Order REST (trigger orders) | http://localhost:8080 |
| Order gRPC | localhost:50051 |
| Inventory gRPC | localhost:50052 |
| Grafana (dashboards) | http://localhost:3000 — anonymous Admin, or `admin`/`admin` |
| Prometheus | http://localhost:9090 |
| Tempo API | http://localhost:3200 |
| Loki API | http://localhost:3100 |
| Kafka (host listener) | localhost:29092 |

Grafana auto-provisions the Prometheus/Loki/Tempo datasources (with Tempo↔Loki
trace-to-log correlation) and two dashboards under the **Choreography Saga**
folder: **Saga SLO Overview** and **Trace & Log Correlation**.

## Happy-path demo

```bash
curl -s localhost:8080/v1/orders \
  -d '{"sku":"SKU-1","quantity":1,"amount":10.0,"customerId":"c1"}'
```

The order should reach `CONFIRMED`. Watch the single Order→Payment→Inventory
trace in Grafana → Explore → Tempo, and jump to correlated logs (AC-2/AC-3).

## Forced-failure / compensation demo (AC-4/AC-5/AC-7)

Three equivalent triggers (tech-stack §10):

1. **Magic amount** — deterministic single payment failure, no restart:
   ```bash
   curl -s localhost:8080/v1/orders \
     -d '{"sku":"SKU-1","quantity":1,"amount":66.06,"customerId":"c1"}'
   ```
2. **Env flag** — every payment fails. Set in `deploy/compose/.env`:
   ```bash
   FORCE_PAYMENT_FAILURE=true docker compose -f deploy/compose/docker-compose.yml up -d payment
   ```
3. **Poison SKU** — exercises the full reverse chain (release → refund → cancel)
   at the inventory stage:
   ```bash
   curl -s localhost:8080/v1/orders \
     -d '{"sku":"SKU-DEADBEEF","quantity":1,"amount":10.0,"customerId":"c1"}'
   ```

The order ends `CANCELLED`, no stock stays reserved, no payment stays captured.
The **Saga SLO Overview** dashboard's compensation/payment-failure panels light
up and the `HighCompensationRate` / `HighPaymentFailureRate` Prometheus alerts
fire (AC-7).

Tear down: `docker compose -f deploy/compose/docker-compose.yml down -v`.

## Kubernetes

Two options, both deploying **only the three services** — Kafka, PostgreSQL and
the observability stack are treated as **external dependencies** (point them at
in-cluster operators or managed endpoints via the config below).

### Helm

```bash
helm lint deploy/helm/choreography-saga
helm install saga deploy/helm/choreography-saga \
  --set global.kafkaBrokers=kafka:9092 \
  --set global.otlpEndpoint=http://otel-collector:4317 \
  --set global.otlpEndpointNoScheme=otel-collector:4317
```

Parameterized in `values.yaml`: images, replica counts, DB DSNs (as Secrets),
Kafka/OTLP endpoints, and `payment.forcePaymentFailure`.

### Plain manifests

```bash
kubectl apply -f deploy/k8s/
```

Edit `deploy/k8s/10-config.yaml` (ConfigMap + Secrets) to match your cluster's
Kafka / Postgres / OTLP endpoints before applying.

## CI

`.github/workflows/ci.yml` builds+tests all three services (Go, Python, C++
core+tests), builds the three multi-stage images, and validates the Compose
file, Helm chart, K8s manifests, Prometheus rules, and OTel Collector config.
