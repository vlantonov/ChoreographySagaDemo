// JSON event envelope shared across services (tech-stack §5.1).
// Depends only on nlohmann/json so it can be unit tested without Kafka or a
// database.
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace inventory::event {

inline constexpr int kSchemaVersion = 1;

// Envelope is the CloudEvents-inspired JSON wrapper carried on every Kafka
// message (tech-stack §5.1). data holds the event-type-specific payload.
struct Envelope {
  std::string event_id;
  std::string event_type;
  int schema_version = kSchemaVersion;
  std::string occurred_at;
  std::string saga_id;
  std::string idempotency_key;
  std::string traceparent;
  nlohmann::json data = nlohmann::json::object();

  // to_json serialises the envelope to its canonical wire form.
  [[nodiscard]] std::string to_json() const;
};

// make_envelope builds an envelope, generating eventId, idempotencyKey, and
// occurredAt when not supplied.
[[nodiscard]] Envelope make_envelope(std::string event_type, std::string saga_id,
                                     nlohmann::json data, std::string traceparent);

// parse deserialises a Kafka message body into an Envelope.
[[nodiscard]] Envelope parse(std::string_view raw);

}  // namespace inventory::event
