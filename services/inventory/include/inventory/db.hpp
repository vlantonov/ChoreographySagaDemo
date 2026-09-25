// PostgreSQL persistence for the Inventory service (tech-stack §4, §8).
// Implements the application Repository and the outbox Repo over libpqxx.
#pragma once

#include <memory>
#include <string>

#include "inventory/outbox.hpp"
#include "inventory/repository.hpp"

namespace pqxx {
class connection;
}

namespace inventory::db {

// Database owns a libpqxx connection and implements both the transactional
// Repository (reserve/release) and the relay-side outbox Repo. Access is
// serialised with an internal mutex; the demo runs a single replica.
class Database : public repo::Repository, public outbox::Repo {
 public:
  explicit Database(const std::string& url);
  ~Database() override;

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // repo::Repository
  repo::ReserveOutcome reserve(
      const std::string& saga_id, const std::string& idempotency_key,
      const std::string& sku, int quantity,
      const repo::EventBuilder<repo::ReserveOutcome>& build_event) override;
  repo::ReleaseOutcome release(
      const std::string& saga_id, const std::string& reservation_id,
      const std::string& idempotency_key, const std::string& consumer,
      const repo::EventBuilder<repo::ReleaseOutcome>& build_event) override;

  // outbox::Repo
  std::vector<outbox::Record> fetch_pending(int limit) override;
  void mark_published(const std::string& id) override;
  void mark_failed(const std::string& id) override;
  int64_t count_pending() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace inventory::db
