#include "inventory/outbox.hpp"

#include <chrono>

#include "inventory/obs.hpp"

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
    if (rec.created_at.time_since_epoch().count() != 0) {
      const double lag = std::chrono::duration<double>(
                             std::chrono::system_clock::now() - rec.created_at)
                             .count();
      obs::record_outbox_publish_lag(lag);
    }
    ++published;
  }
  return published;
}

int64_t Relay::pending_count() { return repo_.count_pending(); }

}  // namespace inventory::outbox
