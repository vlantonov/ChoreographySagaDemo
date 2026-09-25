#!/usr/bin/env bash
#
# run-demo.sh — happy-path saga demo (AC-1, AC-2, AC-3).
#
# Submits one normal order to the running Docker Compose stack via the Order
# service REST gateway (grpc-gateway, POST /v1/orders), then polls GetOrder
# until the saga reaches CONFIRMED and prints the correlation id (order_id ==
# sagaId) plus the W3C trace_id so the reader can open the trace in Grafana/Tempo.
#
# Prerequisites: the stack must already be up:
#   docker compose -f deploy/compose/docker-compose.yml up --build
#
# The submission interface and ports mirror deploy/compose/docker-compose.yml
# (order service publishes REST on host :8080) and deploy/README.md.
#
set -euo pipefail

# --- Configuration (override via environment) --------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${COMPOSE_FILE:-${REPO_ROOT}/deploy/compose/docker-compose.yml}"

ORDER_URL="${ORDER_URL:-http://localhost:8080}"   # Order REST gateway (compose :8080)
GRAFANA_URL="${GRAFANA_URL:-http://localhost:3000}" # Grafana (compose :3000)
TEMPO_URL="${TEMPO_URL:-http://localhost:3200}"     # Tempo query API (compose :3200)

DEMO_SKU="${DEMO_SKU:-SKU-1}"          # valid, in-stock SKU
DEMO_QTY="${DEMO_QTY:-1}"
DEMO_AMOUNT="${DEMO_AMOUNT:-10.0}"     # ordinary amount (NOT the 66.06 magic value)
DEMO_CUSTOMER="${DEMO_CUSTOMER:-c1}"

POLL_TIMEOUT="${POLL_TIMEOUT:-60}"     # seconds to wait for a terminal state
POLL_INTERVAL="${POLL_INTERVAL:-2}"    # seconds between polls
TARGET_STATUS="CONFIRMED"

# --- Preflight: required tools ----------------------------------------------
need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: '$1' is required but not installed. Please install it and retry." >&2
    exit 127
  }
}
need curl
need jq

# --- Preflight: stack reachable ---------------------------------------------
# Any HTTP response (even 404 for a missing order) proves the gateway is up;
# only a connection failure (empty code) means the stack is down.
if ! curl -s -o /dev/null -w '%{http_code}' "${ORDER_URL}/v1/orders/preflight" 2>/dev/null | grep -qE '^[0-9]{3}$'; then
  echo "ERROR: Order service is not reachable at ${ORDER_URL}." >&2
  echo "       Start the stack first:" >&2
  echo "         docker compose -f ${COMPOSE_FILE} up --build" >&2
  exit 1
fi

echo "==> Happy-path demo — submitting a normal order"
echo "    endpoint : POST ${ORDER_URL}/v1/orders"
echo "    payload  : sku=${DEMO_SKU} quantity=${DEMO_QTY} amount=${DEMO_AMOUNT} customerId=${DEMO_CUSTOMER}"

# --- Submit the order (REST via grpc-gateway) --------------------------------
create_body="$(jq -nc \
  --arg sku "${DEMO_SKU}" \
  --argjson qty "${DEMO_QTY}" \
  --argjson amount "${DEMO_AMOUNT}" \
  --arg customer "${DEMO_CUSTOMER}" \
  '{sku:$sku, quantity:$qty, amount:$amount, customerId:$customer}')"

create_resp="$(curl -fsS -X POST "${ORDER_URL}/v1/orders" \
  -H 'Content-Type: application/json' \
  -d "${create_body}")"

order_id="$(printf '%s' "${create_resp}" | jq -r '.orderId // empty')"
init_status="$(printf '%s' "${create_resp}" | jq -r '.status // empty')"

if [ -z "${order_id}" ]; then
  echo "ERROR: could not parse orderId from CreateOrder response:" >&2
  printf '%s\n' "${create_resp}" >&2
  exit 1
fi

echo "    created  : order_id=${order_id} status=${init_status}"
echo "    (order_id == sagaId == correlation id for logs/traces)"

# --- Poll GetOrder until CONFIRMED or timeout --------------------------------
echo "==> Polling GetOrder until status=${TARGET_STATUS} (timeout ${POLL_TIMEOUT}s)"
deadline=$(( $(date +%s) + POLL_TIMEOUT ))
status=""
while [ "$(date +%s)" -lt "${deadline}" ]; do
  status="$(curl -fsS "${ORDER_URL}/v1/orders/${order_id}" | jq -r '.status // empty')"
  echo "    status=${status}"
  if [ "${status}" = "${TARGET_STATUS}" ]; then
    break
  fi
  if [ "${status}" = "CANCELLED" ]; then
    echo "ERROR: order ${order_id} was CANCELLED — the happy path unexpectedly compensated." >&2
    exit 1
  fi
  sleep "${POLL_INTERVAL}"
done

if [ "${status}" != "${TARGET_STATUS}" ]; then
  echo "ERROR: order ${order_id} did not reach ${TARGET_STATUS} within ${POLL_TIMEOUT}s (last=${status})." >&2
  exit 1
fi

# --- Surface the trace_id for Grafana/Tempo ----------------------------------
# Best-effort: pull the active trace_id from the Order service's structured JSON
# logs (obs.go injects trace_id/span_id on every record). Requires docker compose.
trace_id=""
if command -v docker >/dev/null 2>&1; then
  trace_id="$(docker compose -f "${COMPOSE_FILE}" logs --no-color order 2>/dev/null \
    | grep -F "${order_id}" \
    | grep -oE '"trace_id":"[0-9a-f]+"' \
    | grep -oE '[0-9a-f]{16,}' \
    | grep -vE '^0+$' \
    | head -n1 || true)"
fi

echo
echo "PASS: order ${order_id} reached ${TARGET_STATUS}."
echo "----------------------------------------------------------------------"
echo "  correlation id (sagaId/orderId): ${order_id}"
if [ -n "${trace_id}" ]; then
  echo "  trace_id                       : ${trace_id}"
else
  echo "  trace_id                       : (unavailable from logs; search by sagaId below)"
fi
echo
echo "  View the end-to-end trace and correlated logs:"
echo "    Grafana        : ${GRAFANA_URL}  (Explore -> Tempo)"
if [ -n "${trace_id}" ]; then
  echo "    Tempo trace    : ${TEMPO_URL}/api/traces/${trace_id}"
fi
echo "    Loki logs      : ${GRAFANA_URL}  (Explore -> Loki, filter {} |= \"${order_id}\")"
echo "----------------------------------------------------------------------"
