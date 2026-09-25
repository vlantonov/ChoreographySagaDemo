// Application layer: the synchronous reservation step and its compensation
// (ARCHITECTURE §3, §7, §8). Wires the domain rules to the repository and the
// transactional outbox. Performs no I/O of its own beyond injected
// collaborators, so it is unit-testable with in-memory fakes.
#pragma once

#include <string>
#include <vector>

#include "inventory/domain.hpp"
#include "inventory/envelope.hpp"
#include "inventory/repository.hpp"

namespace inventory::app {

// Produced topics (tech-stack §5.3).
inline constexpr const char* kTopicInventoryReserved = "inventory.reserved";
inline constexpr const char* kTopicInventoryReservationFailed =
    "inventory.reservation_failed";
inline constexpr const char* kTopicInventoryReleased = "inventory.released";

// Consumed topic (ARCHITECTURE §2, §8): compensation trigger.
inline constexpr const char* kTopicOrderCancelled = "order.cancelled";

// Idempotency consumer labels (part of the processed_messages composite key).
inline constexpr const char* kConsumerReserve = "inventory-reserve";
inline constexpr const char* kConsumerRelease = "inventory-release";
inline constexpr const char* kConsumerOrderCancelled = "inventory-order-cancelled";

// ReserveResult is the typed outcome returned over gRPC (tech-stack §6).
struct ReserveResult {
  domain::ReserveStatus status = domain::ReserveStatus::Unspecified;
  std::string reservation_id;
  std::string message;
};

// InventoryApp implements ReserveStock, ReleaseStock, and the order.cancelled
// compensation handler.
class InventoryApp {
 public:
  explicit InventoryApp(repo::Repository& repository);

  // Topics consumed from Kafka.
  static std::vector<std::string> consumed_topics();

  // reserve_stock is the synchronous gRPC leg (tech-stack §6, ARCHITECTURE §7).
  // On success it persists a reservation and emits inventory.reserved; on the
  // poison SKU / insufficient stock it emits inventory.reservation_failed.
  ReserveResult reserve_stock(const std::string& saga_id,
                              const std::string& idempotency_key,
                              const std::string& sku, int quantity,
                              const std::string& traceparent);

  // release_stock restores stock and emits inventory.released when a reservation
  // was actually released (compensation, FR-3, FR-21).
  bool release_stock(const std::string& saga_id, const std::string& reservation_id,
                     const std::string& idempotency_key,
                     const std::string& traceparent);

  // handle_order_cancelled is the Kafka compensation handler; it releases the
  // saga's reservation idempotently.
  void handle_order_cancelled(const event::Envelope& env,
                              const std::string& in_traceparent);

 private:
  repo::Repository& repo_;
};

}  // namespace inventory::app
