#include "inventory/db.hpp"

#include <mutex>
#include <optional>

#include <pqxx/pqxx>

#include "inventory/domain.hpp"

namespace inventory::db {

struct Database::Impl {
  pqxx::connection conn;
  std::mutex mutex;
  explicit Impl(const std::string& url) : conn(url) {}
};

Database::Database(const std::string& url) : impl_(std::make_unique<Impl>(url)) {}
Database::~Database() = default;

namespace {

// mark_processed inserts a dedupe marker; returns false if already processed.
bool mark_processed(pqxx::work& tx, const std::string& key, const std::string& consumer) {
  const pqxx::result r = tx.exec_params(
      "INSERT INTO processed_messages (idempotency_key, consumer) "
      "VALUES ($1, $2) ON CONFLICT DO NOTHING RETURNING idempotency_key",
      key, consumer);
  return !r.empty();
}

void insert_outbox(pqxx::work& tx, const outbox::Insert& ob) {
  tx.exec_params(
      "INSERT INTO outbox "
      "(id, aggregate_type, aggregate_id, event_type, topic, payload, headers, status) "
      "VALUES (gen_random_uuid(), $1, $2, $3, $4, CAST($5 AS JSONB), "
      "jsonb_build_object('traceparent', CAST($6 AS TEXT)), 'PENDING')",
      ob.aggregate_type, ob.aggregate_id, ob.event_type, ob.topic, ob.payload,
      ob.traceparent);
}

}  // namespace

repo::ReserveOutcome Database::reserve(
    const std::string& saga_id, const std::string& idempotency_key,
    const std::string& sku, int quantity,
    const repo::EventBuilder<repo::ReserveOutcome>& build_event) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  repo::ReserveOutcome outcome;

  // Idempotency keyed on the request's idempotency_key (tech-stack §8.3).
  if (!mark_processed(tx, idempotency_key, "inventory-reserve")) {
    outcome.duplicate = true;
    const pqxx::row existing = tx.exec_params1(
        "SELECT COALESCE((SELECT id::text FROM reservations "
        "WHERE saga_id = $1 AND status = 'RESERVED'), '')",
        saga_id);
    const std::string id = existing[0].as<std::string>();
    if (!id.empty()) {
      outcome.status = domain::ReserveStatus::Reserved;
      outcome.reservation_id = id;
    } else {
      outcome.status = domain::ReserveStatus::InsufficientStock;
    }
    tx.commit();
    return outcome;  // duplicate: no new event
  }

  // Lock the stock row and decide the outcome inside the transaction (FR-9).
  const pqxx::result stock = tx.exec_params(
      "SELECT available FROM stock WHERE sku = $1 FOR UPDATE", sku);
  const int available = stock.empty() ? 0 : stock[0][0].as<int>();

  if (domain::is_poison_sku(sku) || available < quantity) {
    outcome.status = domain::ReserveStatus::InsufficientStock;
  } else {
    tx.exec_params(
        "UPDATE stock SET available = available - $2, reserved = reserved + $2 "
        "WHERE sku = $1",
        sku, quantity);
    const pqxx::row res = tx.exec_params1(
        "INSERT INTO reservations (id, saga_id, sku, quantity, status) "
        "VALUES (gen_random_uuid(), $1, $2, $3, 'RESERVED') "
        "ON CONFLICT (saga_id) DO UPDATE SET saga_id = EXCLUDED.saga_id "
        "RETURNING id::text",
        saga_id, sku, quantity);
    outcome.status = domain::ReserveStatus::Reserved;
    outcome.reservation_id = res[0].as<std::string>();
  }

  insert_outbox(tx, build_event(outcome));
  tx.commit();
  return outcome;
}

repo::ReleaseOutcome Database::release(
    const std::string& saga_id, const std::string& /*reservation_id*/,
    const std::string& idempotency_key, const std::string& consumer,
    const repo::EventBuilder<repo::ReleaseOutcome>& build_event) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  repo::ReleaseOutcome outcome;

  if (!mark_processed(tx, idempotency_key, consumer)) {
    outcome.duplicate = true;
    tx.commit();
    return outcome;
  }

  const pqxx::result released = tx.exec_params(
      "UPDATE reservations SET status = 'RELEASED' "
      "WHERE saga_id = $1 AND status = 'RESERVED' RETURNING sku, quantity",
      saga_id);
  if (!released.empty()) {
    const std::string sku = released[0][0].as<std::string>();
    const int quantity = released[0][1].as<int>();
    tx.exec_params(
        "UPDATE stock SET available = available + $2, reserved = reserved - $2 "
        "WHERE sku = $1",
        sku, quantity);
    outcome.released = true;
    insert_outbox(tx, build_event(outcome));
  }
  tx.commit();
  return outcome;
}

std::vector<outbox::Record> Database::fetch_pending(int limit) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  const pqxx::result rows = tx.exec_params(
      "SELECT id::text, topic, aggregate_id, payload::text, "
      "COALESCE(headers->>'traceparent', '') "
      "FROM outbox WHERE status = 'PENDING' ORDER BY created_at "
      "FOR UPDATE SKIP LOCKED LIMIT $1",
      limit);
  std::vector<outbox::Record> out;
  out.reserve(rows.size());
  for (const auto& r : rows) {
    out.push_back(outbox::Record{
        r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
        r[3].as<std::string>(), r[4].as<std::string>()});
  }
  tx.commit();
  return out;
}

void Database::mark_published(const std::string& id) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  tx.exec_params(
      "UPDATE outbox SET status = 'PUBLISHED', published_at = now() WHERE id = $1", id);
  tx.commit();
}

void Database::mark_failed(const std::string& id) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  tx.exec_params("UPDATE outbox SET attempts = attempts + 1 WHERE id = $1", id);
  tx.commit();
}

int64_t Database::count_pending() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  pqxx::work tx(impl_->conn);
  const pqxx::row row =
      tx.exec1("SELECT count(*) FROM outbox WHERE status = 'PENDING'");
  const auto n = row[0].as<int64_t>();
  tx.commit();
  return n;
}

}  // namespace inventory::db
