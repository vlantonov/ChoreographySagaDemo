// Transactional-outbox relay for the Inventory service (tech-stack §8.2,
// FR-9..FR-11). The relay depends only on small abstract interfaces so it is
// unit-testable with in-memory fakes (no Kafka, no Postgres).
#pragma once

#include <string>
#include <vector>

namespace inventory::outbox {

// Insert is an event to enqueue transactionally with a business write (FR-9).
struct Insert {
  std::string aggregate_type;
  std::string aggregate_id;
  std::string event_type;
  std::string topic;
  std::string payload;
  std::string traceparent;
};

// Record is one pending outbox row read by the relay (tech-stack §8.1).
struct Record {
  std::string id;
  std::string topic;
  std::string aggregate_id;
  std::string payload;
  std::string traceparent;
};

// Repo reads and marks outbox rows. The SQL implementation selects PENDING rows
// with FOR UPDATE SKIP LOCKED so concurrent/restarted relays never double-send.
class Repo {
 public:
  virtual ~Repo() = default;
  virtual std::vector<Record> fetch_pending(int limit) = 0;
  virtual void mark_published(const std::string& id) = 0;
  virtual void mark_failed(const std::string& id) = 0;
};

// Publisher sends a single record to the event backbone (implemented by the
// Kafka producer).
class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual void publish(const std::string& topic, const std::string& key,
                       const std::string& payload, const std::string& traceparent) = 0;
};

// Relay polls the outbox and publishes pending rows at-least-once (AC-8).
class Relay {
 public:
  Relay(Repo& repo, Publisher& publisher, int batch_size = 100);

  // drain_once publishes one batch of PENDING rows. Returns the number of rows
  // published. A publish failure leaves the row PENDING for a later retry and
  // is rethrown so the caller can back off.
  int drain_once();

 private:
  Repo& repo_;
  Publisher& publisher_;
  int batch_size_;
};

}  // namespace inventory::outbox
