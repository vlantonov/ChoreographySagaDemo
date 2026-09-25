#!/usr/bin/env bash
#
# force-failure.sh — forced-failure / compensation demo (AC-4, AC-5, AC-6, AC-7).
#
# Drives the running Docker Compose stack through a compensating (rollback) saga
# and surfaces the evidence so the reader can see the reverse-order rollback in
# logs and traces. Two scenarios (tech-stack §10, deploy/README.md):
#
#   payment   — payment-stage failure via magic amount 66.06 (deterministic;
#               or set FORCE_PAYMENT_FAILURE=true on the payment service). The
#               order is cancelled directly: payment.failed -> order.cancelled.
#   inventory — inventory-stage failure via poison SKU "SKU-DEADBEEF" (zero
#               stock -> ReserveStock INSUFFICIENT_STOCK). Exercises the full
#               reverse chain: inventory.reservation_failed -> payment.refunded
#               -> order.cancelled (-> inventory.released).
#
# Usage:
#   scripts/force-failure.sh [payment|inventory|both]   (default: both)
#
# Prerequisites: the stack must already be up:
#   docker compose -f deploy/compose/docker-compose.yml up --build
#
set -euo pipefail

# --- Configuration (override via environment) --------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${COMPOSE_FILE:-${REPO_ROOT}/deploy/compose/docker-compose.yml}"

ORDER_URL="${ORDER_URL:-http://localhost:8080}"     # Order REST gateway (compose :8080)
GRAFANA_URL="${GRAFANA_URL:-http://localhost:3000}" # Grafana (compose :3000)
TEMPO_URL="${TEMPO_URL:-http://localhost:3200}"     # Tempo query API (compose :3200)

MAGIC_AMOUNT="${MAGIC_AMOUNT:-66.06}"     # payment-stage forced failure (tech-stack §10)
POISON_SKU="${POISON_SKU:-SKU-DEADBEEF}"  # inventory-stage forced failure (tech-stack §10)
GOOD_SKU="${GOOD_SKU:-SKU-1}"
GOOD_AMOUNT="${GOOD_AMOUNT:-10.0}"
DEMO_QTY="${DEMO_QTY:-1}"
DEMO_CUSTOMER="${DEMO_CUSTOMER:-c1}"

POLL_TIMEOUT="${POLL_TIMEOUT:-90}"        # seconds to wait for CANCELLED
POLL_INTERVAL="${POLL_INTERVAL:-2}"       # seconds between polls
TARGET_STATUS="CANCELLED"

SCENARIO="${1:-both}"

# --- Preflight: required tools ----------------------------------------------
need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: '$1' is required but not installed. Please install it and retry." >&2
    exit 127
  }
}
need curl
need jq
HAVE_DOCKER=1
command -v docker >/dev/null 2>&1 || {
  HAVE_DOCKER=0
  echo "NOTE: 'docker' not found — will poll order status but cannot tail service logs for evidence." >&2
}

# --- Preflight: stack reachable ---------------------------------------------
if ! curl -s -o /dev/null -w '%{http_code}' "${ORDER_URL}/v1/orders/preflight" 2>/dev/null | grep -qE '^[0-9]{3}$'; then
  echo "ERROR: Order service is not reachable at ${ORDER_URL}." >&2
  echo "       Start the stack first:" >&2
  echo "         docker compose -f ${COMPOSE_FILE} up --build" >&2
  exit 1
fi

# submit_order <sku> <amount> -> echoes the created order_id
submit_order() {
  sku="$1"; amount="$2"
  body="$(jq -nc \
    --arg sku "${sku}" \
    --argjson qty "${DEMO_QTY}" \
    --argjson amount "${amount}" \
    --arg customer "${DEMO_CUSTOMER}" \
    '{sku:$sku, quantity:$qty, amount:$amount, customerId:$customer}')"
  resp="$(curl -fsS -X POST "${ORDER_URL}/v1/orders" \
    -H 'Content-Type: application/json' -d "${body}")"
  oid="$(printf '%s' "${resp}" | jq -r '.orderId // empty')"
  if [ -z "${oid}" ]; then
    echo "ERROR: could not parse orderId from CreateOrder response:" >&2
    printf '%s\n' "${resp}" >&2
    return 1
  fi
  printf '%s' "${oid}"
}

# poll_until_cancelled <order_id> -> 0 if CANCELLED, 1 on timeout/confirm
poll_until_cancelled() {
  oid="$1"
  deadline=$(( $(date +%s) + POLL_TIMEOUT ))
  status=""
  while [ "$(date +%s)" -lt "${deadline}" ]; do
    status="$(curl -fsS "${ORDER_URL}/v1/orders/${oid}" | jq -r '.status // empty')"
    echo "    status=${status}"
    if [ "${status}" = "${TARGET_STATUS}" ]; then
      return 0
    fi
    if [ "${status}" = "CONFIRMED" ]; then
      echo "ERROR: order ${oid} reached CONFIRMED — expected compensation to CANCELLED." >&2
      return 1
    fi
    sleep "${POLL_INTERVAL}"
  done
  echo "ERROR: order ${oid} did not reach ${TARGET_STATUS} within ${POLL_TIMEOUT}s (last=${status})." >&2
  return 1
}

# trace_id_for <order_id> -> echoes the first non-zero trace_id seen in logs
trace_id_for() {
  [ "${HAVE_DOCKER}" -eq 1 ] || return 0
  docker compose -f "${COMPOSE_FILE}" logs --no-color order payment inventory 2>/dev/null \
    | grep -F "$1" \
    | grep -oE '"trace_id":"[0-9a-f]+"' \
    | grep -oE '[0-9a-f]{16,}' \
    | grep -vE '^0+$' \
    | head -n1 || true
}

# show_compensation_evidence <order_id> — grep the services' logs for the
# rollback events tied to this saga (payment.failed / reservation_failed /
# payment.refunded / order.cancelled / inventory.released).
show_compensation_evidence() {
  oid="$1"
  if [ "${HAVE_DOCKER}" -eq 0 ]; then
    echo "    (docker unavailable — inspect logs manually, filtering for ${oid})"
    return 0
  fi
  echo "    compensation/rollback log lines for saga ${oid}:"
  # Match the compensation event/topic names and the cancel/refund phrases the
  # three services emit; keep only lines that also carry this saga's id.
  matches="$(docker compose -f "${COMPOSE_FILE}" logs --no-color order payment inventory 2>/dev/null \
    | grep -F "${oid}" \
    | grep -iE 'payment\.failed|reservation_failed|payment\.refunded|order\.cancelled|inventory\.released|payment failed|payment refunded|cancelled|OrderCancelled' \
    || true)"
  if [ -n "${matches}" ]; then
    printf '%s\n' "${matches}" | sed 's/^/      | /'
  else
    echo "      (no matching lines yet; events may still be propagating — re-run logs to confirm)"
  fi
}

# run_scenario <name> <sku> <amount> <description>
run_scenario() {
  name="$1"; sku="$2"; amount="$3"; desc="$4"
  echo "======================================================================"
  echo "==> Scenario: ${name} — ${desc}"
  echo "    endpoint : POST ${ORDER_URL}/v1/orders"
  echo "    payload  : sku=${sku} quantity=${DEMO_QTY} amount=${amount} customerId=${DEMO_CUSTOMER}"

  oid="$(submit_order "${sku}" "${amount}")"
  echo "    created  : order_id=${oid} (== sagaId == correlation id)"

  echo "==> Polling GetOrder until status=${TARGET_STATUS} (timeout ${POLL_TIMEOUT}s)"
  if ! poll_until_cancelled "${oid}"; then
    return 1
  fi

  echo "PASS: order ${oid} reached ${TARGET_STATUS} (compensation completed)."
  show_compensation_evidence "${oid}"

  tid="$(trace_id_for "${oid}")"
  echo "----------------------------------------------------------------------"
  echo "  correlation id (sagaId/orderId): ${oid}"
  if [ -n "${tid}" ]; then
    echo "  trace_id                       : ${tid}"
    echo "  Tempo trace                    : ${TEMPO_URL}/api/traces/${tid}"
  else
    echo "  trace_id                       : (unavailable from logs; search Tempo by sagaId)"
  fi
  echo "  Grafana                        : ${GRAFANA_URL}  (Explore -> Tempo / Loki)"
  echo "  Loki logs                      : filter {} |= \"${oid}\""
  echo "----------------------------------------------------------------------"
  return 0
}

rc=0
case "${SCENARIO}" in
  payment)
    run_scenario "payment-stage failure" "${GOOD_SKU}" "${MAGIC_AMOUNT}" \
      "magic amount ${MAGIC_AMOUNT} -> payment.failed -> order.cancelled" || rc=1
    ;;
  inventory)
    run_scenario "inventory-stage failure" "${POISON_SKU}" "${GOOD_AMOUNT}" \
      "poison SKU ${POISON_SKU} -> reservation_failed -> refund -> cancel" || rc=1
    ;;
  both)
    run_scenario "payment-stage failure" "${GOOD_SKU}" "${MAGIC_AMOUNT}" \
      "magic amount ${MAGIC_AMOUNT} -> payment.failed -> order.cancelled" || rc=1
    echo
    run_scenario "inventory-stage failure" "${POISON_SKU}" "${GOOD_AMOUNT}" \
      "poison SKU ${POISON_SKU} -> reservation_failed -> refund -> cancel" || rc=1
    ;;
  *)
    echo "ERROR: unknown scenario '${SCENARIO}'. Use: payment | inventory | both" >&2
    exit 2
    ;;
esac

echo
if [ "${rc}" -eq 0 ]; then
  echo "ALL SCENARIOS PASSED: every order reached ${TARGET_STATUS} with compensation evidence."
else
  echo "ONE OR MORE SCENARIOS FAILED — see errors above." >&2
fi
exit "${rc}"
