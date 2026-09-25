// Repository abstraction for the Inventory service (ARCHITECTURE §3, §11).
// Every method wraps its business write and the outbox insert in one local
// transaction (FR-9). The interface is abstract so unit tests inject an
// in-memory fake without a live database.
#pragma once

#include <functional>
#include <string>

#include "inventory/domain.hpp"
#include "inventory/outbox.hpp"

namespace inventory::repo {

// ReserveOutcome is the result of a stock reservation attempt.
struct ReserveOutcome {
  domain::ReserveStatus status = domain::ReserveStatus::Unspecified;
  std::string reservation_id;
  bool duplicate = false;  // true when the idempotency key was already processed
};

// ReleaseOutcome is the result of a stock release (compensation).
struct ReleaseOutcome {
  bool released = false;    // true when a RESERVED reservation was restored
  bool duplicate = false;   // true when the idempotency key was already processed
};

// EventBuilder produces the outbox event to enqueue, given the decided outcome.
// The repository invokes it inside the transaction so the business write and the
// event insert commit atomically (FR-9).
template <typename Outcome>
using EventBuilder = std::function<outbox::Insert(const Outcome&)>;

// Repository is the persistence surface the application layer depends on.
class Repository {
 public:
  virtual ~Repository() = default;

  // reserve checks and decrements stock idempotently. The poison SKU and
  // insufficient stock both yield InsufficientStock without a reservation row.
  virtual ReserveOutcome reserve(const std::string& saga_id,
                                 const std::string& idempotency_key,
                                 const std::string& sku, int quantity,
                                 const EventBuilder<ReserveOutcome>& build_event) = 0;

  // release restores stock and marks the reservation RELEASED idempotently.
  virtual ReleaseOutcome release(const std::string& saga_id,
                                 const std::string& reservation_id,
                                 const std::string& idempotency_key,
                                 const std::string& consumer,
                                 const EventBuilder<ReleaseOutcome>& build_event) = 0;
};

}  // namespace inventory::repo
