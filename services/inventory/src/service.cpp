#include "inventory/service.hpp"

#include <utility>

#include "inventory/obs.hpp"

namespace inventory::app {

namespace {

// reserved_event builds the inventory.reserved envelope payload (happy path).
outbox::Insert reserved_event(const std::string& saga_id, const std::string& sku,
                              int quantity, const repo::ReserveOutcome& outcome,
                              const std::string& traceparent) {
  auto env = event::make_envelope(
      "InventoryReserved", saga_id,
      {
          {"orderId", saga_id},
          {"sagaId", saga_id},
          {"sku", sku},
          {"quantity", quantity},
          {"reservationId", outcome.reservation_id},
          {"status", "RESERVED"},
      },
      traceparent);
  return outbox::Insert{"inventory", saga_id, "InventoryReserved",
                        kTopicInventoryReserved, env.to_json(), traceparent};
}

// failed_event builds the inventory.reservation_failed envelope payload.
outbox::Insert failed_event(const std::string& saga_id, const std::string& sku,
                            int quantity, const std::string& reason,
                            const std::string& traceparent) {
  auto env = event::make_envelope(
      "InventoryReservationFailed", saga_id,
      {
          {"orderId", saga_id},
          {"sagaId", saga_id},
          {"sku", sku},
          {"quantity", quantity},
          {"reason", reason},
          {"status", "INSUFFICIENT_STOCK"},
      },
      traceparent);
  return outbox::Insert{"inventory", saga_id, "InventoryReservationFailed",
                        kTopicInventoryReservationFailed, env.to_json(), traceparent};
}

// released_event builds the inventory.released envelope payload (compensation).
outbox::Insert released_event(const std::string& saga_id,
                              const std::string& reservation_id,
                              const std::string& traceparent) {
  auto env = event::make_envelope(
      "InventoryReleased", saga_id,
      {
          {"orderId", saga_id},
          {"sagaId", saga_id},
          {"reservationId", reservation_id},
          {"status", "RELEASED"},
      },
      traceparent);
  return outbox::Insert{"inventory", saga_id, "InventoryReleased",
                        kTopicInventoryReleased, env.to_json(), traceparent};
}

}  // namespace

InventoryApp::InventoryApp(repo::Repository& repository) : repo_(repository) {}

std::vector<std::string> InventoryApp::consumed_topics() {
  return {kTopicOrderCancelled};
}

ReserveResult InventoryApp::reserve_stock(const std::string& saga_id,
                                          const std::string& idempotency_key,
                                          const std::string& sku, int quantity,
                                          const std::string& traceparent) {
  // The event to emit depends on the outcome decided inside the transaction, so
  // the payload is built lazily via this callback (FR-9 atomicity).
  auto build_event = [&](const repo::ReserveOutcome& outcome) -> outbox::Insert {
    if (outcome.status == domain::ReserveStatus::Reserved) {
      return reserved_event(saga_id, sku, quantity, outcome, traceparent);
    }
    const std::string reason =
        domain::is_poison_sku(sku) ? "poison SKU forced failure" : "insufficient stock";
    return failed_event(saga_id, sku, quantity, reason, traceparent);
  };

  const repo::ReserveOutcome outcome =
      repo_.reserve(saga_id, idempotency_key, sku, quantity, build_event);

  ReserveResult result;
  result.status = outcome.status;
  result.reservation_id = outcome.reservation_id;
  if (outcome.status == domain::ReserveStatus::Reserved) {
    result.message = outcome.duplicate ? "already reserved" : "reserved";
    obs::log_info("reserve_stock reserved",
                  {{"saga_id", saga_id}, {"sku", sku}, {"duplicate", outcome.duplicate}});
  } else {
    result.message =
        domain::is_poison_sku(sku) ? "poison SKU forced failure" : "insufficient stock";
    obs::log_info("reserve_stock failed",
                  {{"saga_id", saga_id}, {"sku", sku}, {"reason", result.message}});
  }
  return result;
}

bool InventoryApp::release_stock(const std::string& saga_id,
                                 const std::string& reservation_id,
                                 const std::string& idempotency_key,
                                 const std::string& traceparent) {
  auto build_event = [&](const repo::ReleaseOutcome& outcome) -> outbox::Insert {
    (void)outcome;
    return released_event(saga_id, reservation_id, traceparent);
  };

  const repo::ReleaseOutcome outcome =
      repo_.release(saga_id, reservation_id, idempotency_key, kConsumerRelease, build_event);
  obs::log_info("release_stock",
                {{"saga_id", saga_id}, {"released", outcome.released},
                 {"duplicate", outcome.duplicate}});
  return outcome.released;
}

void InventoryApp::handle_order_cancelled(const event::Envelope& env,
                                          const std::string& in_traceparent) {
  const std::string traceparent = !env.traceparent.empty() ? env.traceparent : in_traceparent;
  const std::string reservation_id = env.data.value("reservationId", "");

  auto build_event = [&](const repo::ReleaseOutcome& outcome) -> outbox::Insert {
    (void)outcome;
    return released_event(env.saga_id, reservation_id, traceparent);
  };

  const repo::ReleaseOutcome outcome = repo_.release(
      env.saga_id, reservation_id, env.idempotency_key, kConsumerOrderCancelled, build_event);
  if (outcome.duplicate) {
    obs::log_info("order.cancelled duplicate ignored", {{"saga_id", env.saga_id}});
    return;
  }
  obs::log_info("order.cancelled handled",
                {{"saga_id", env.saga_id}, {"released", outcome.released}});
}

}  // namespace inventory::app
