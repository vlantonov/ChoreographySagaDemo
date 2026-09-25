// In-memory fakes for the Inventory unit tests (ARCHITECTURE §11). They let the
// application layer and relay be tested without Postgres or Kafka.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "inventory/outbox.hpp"
#include "inventory/repository.hpp"
#include "inventory/service.hpp"

namespace inventory::testing {

// FakeRepository mirrors the transactional semantics of the libpqxx repository:
// idempotency via processed keys, atomic stock decrement + outbox insert.
class FakeRepository : public repo::Repository {
 public:
  struct Reservation {
    std::string id;
    std::string sku;
    int quantity = 0;
    domain::ReservationState state = domain::ReservationState::Reserved;
  };

  std::map<std::string, int> available;                 // sku -> available units
  std::map<std::string, int> reserved_units;            // sku -> reserved units
  std::map<std::string, Reservation> reservations;      // saga_id -> reservation
  std::set<std::string> processed;                      // idempotency_key|consumer
  std::vector<outbox::Insert> emitted;                  // captured outbox events

  explicit FakeRepository(std::map<std::string, int> initial_stock)
      : available(std::move(initial_stock)) {}

  repo::ReserveOutcome reserve(
      const std::string& saga_id, const std::string& idempotency_key,
      const std::string& sku, int quantity,
      const repo::EventBuilder<repo::ReserveOutcome>& build_event) override {
    repo::ReserveOutcome outcome;
    const std::string key = idempotency_key + "|" + app::kConsumerReserve;
    if (processed.count(key) != 0) {
      outcome.duplicate = true;
      const auto it = reservations.find(saga_id);
      if (it != reservations.end() &&
          it->second.state == domain::ReservationState::Reserved) {
        outcome.status = domain::ReserveStatus::Reserved;
        outcome.reservation_id = it->second.id;
      } else {
        outcome.status = domain::ReserveStatus::InsufficientStock;
      }
      return outcome;  // duplicate: no new event
    }
    processed.insert(key);

    if (domain::is_poison_sku(sku) || available[sku] < quantity) {
      outcome.status = domain::ReserveStatus::InsufficientStock;
    } else {
      available[sku] -= quantity;
      reserved_units[sku] += quantity;
      outcome.reservation_id = "res-" + std::to_string(++next_id_);
      reservations[saga_id] =
          Reservation{outcome.reservation_id, sku, quantity,
                      domain::ReservationState::Reserved};
      outcome.status = domain::ReserveStatus::Reserved;
    }
    emitted.push_back(build_event(outcome));
    return outcome;
  }

  repo::ReleaseOutcome release(
      const std::string& saga_id, const std::string& /*reservation_id*/,
      const std::string& idempotency_key, const std::string& consumer,
      const repo::EventBuilder<repo::ReleaseOutcome>& build_event) override {
    repo::ReleaseOutcome outcome;
    const std::string key = idempotency_key + "|" + consumer;
    if (processed.count(key) != 0) {
      outcome.duplicate = true;
      return outcome;
    }
    processed.insert(key);

    const auto it = reservations.find(saga_id);
    if (it != reservations.end() &&
        it->second.state == domain::ReservationState::Reserved) {
      it->second.state = domain::ReservationState::Released;
      available[it->second.sku] += it->second.quantity;
      reserved_units[it->second.sku] -= it->second.quantity;
      outcome.released = true;
      emitted.push_back(build_event(outcome));
    }
    return outcome;
  }

 private:
  int next_id_ = 0;
};

// FakeOutboxRepo / FakePublisher exercise the relay without a broker.
class FakeOutboxRepo : public outbox::Repo {
 public:
  std::vector<outbox::Record> pending;
  std::set<std::string> published;
  std::map<std::string, int> failed;

  std::vector<outbox::Record> fetch_pending(int limit) override {
    std::vector<outbox::Record> out;
    for (const auto& r : pending) {
      if (static_cast<int>(out.size()) >= limit) break;
      if (published.count(r.id) == 0) out.push_back(r);
    }
    return out;
  }
  void mark_published(const std::string& id) override { published.insert(id); }
  void mark_failed(const std::string& id) override { ++failed[id]; }
  int64_t count_pending() override {
    int64_t n = 0;
    for (const auto& r : pending) {
      if (published.count(r.id) == 0) ++n;
    }
    return n;
  }
};

class FakePublisher : public outbox::Publisher {
 public:
  struct Sent {
    std::string topic;
    std::string key;
    std::string payload;
    std::string traceparent;
  };
  std::vector<Sent> sent;
  bool fail_next = false;

  void publish(const std::string& topic, const std::string& key,
               const std::string& payload, const std::string& traceparent) override {
    if (fail_next) {
      fail_next = false;
      throw std::runtime_error("broker unavailable");
    }
    sent.push_back({topic, key, payload, traceparent});
  }
};

}  // namespace inventory::testing
