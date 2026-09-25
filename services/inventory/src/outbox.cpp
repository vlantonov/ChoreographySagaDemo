#include "inventory/outbox.hpp"

namespace inventory::outbox {

Relay::Relay(Repo& repo, Publisher& publisher, int batch_size)
    : repo_(repo), publisher_(publisher), batch_size_(batch_size > 0 ? batch_size : 100) {}

int Relay::drain_once() {
  const std::vector<Record> rows = repo_.fetch_pending(batch_size_);
  int published = 0;
  for (const Record& rec : rows) {
    try {
      publisher_.publish(rec.topic, rec.aggregate_id, rec.payload, rec.traceparent);
    } catch (...) {
      // Leave the row PENDING; a later tick retries (at-least-once, AC-8).
      repo_.mark_failed(rec.id);
      throw;
    }
    repo_.mark_published(rec.id);
    ++published;
  }
  return published;
}

}  // namespace inventory::outbox
