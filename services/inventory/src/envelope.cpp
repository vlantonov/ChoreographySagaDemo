#include "inventory/envelope.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <random>
#include <sstream>

namespace inventory::event {

namespace {

// uuid_v4 generates a random RFC-4122 version-4 UUID string. A thread-local
// engine keeps generation cheap and safe across the gRPC/consumer threads.
std::string uuid_v4() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t hi = dist(engine);
  const std::uint64_t lo = dist(engine);

  std::array<std::uint8_t, 16> bytes{};
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>(hi >> (8 * i));
    bytes[i + 8] = static_cast<std::uint8_t>(lo >> (8 * i));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 1

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[bytes[i] >> 4]);
    out.push_back(kHex[bytes[i] & 0x0F]);
  }
  return out;
}

// rfc3339_now returns the current UTC time as an RFC-3339 timestamp.
std::string rfc3339_now() {
  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
  const std::time_t t = std::chrono::system_clock::to_time_t(secs);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::array<char, 32> buf{};
  std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%S", &tm);
  std::ostringstream oss;
  oss << buf.data() << '.';
  oss.width(3);
  oss.fill('0');
  oss << ms << 'Z';
  return oss.str();
}

}  // namespace

std::string Envelope::to_json() const {
  nlohmann::json obj{
      {"eventId", event_id},
      {"eventType", event_type},
      {"schemaVersion", schema_version},
      {"occurredAt", occurred_at},
      {"sagaId", saga_id},
      {"idempotencyKey", idempotency_key},
      {"traceparent", traceparent},
      {"data", data},
  };
  return obj.dump();
}

Envelope make_envelope(std::string event_type, std::string saga_id, nlohmann::json data,
                       std::string traceparent) {
  Envelope env;
  env.event_id = uuid_v4();
  env.event_type = std::move(event_type);
  env.schema_version = kSchemaVersion;
  env.occurred_at = rfc3339_now();
  env.saga_id = std::move(saga_id);
  env.idempotency_key = uuid_v4();
  env.traceparent = std::move(traceparent);
  env.data = std::move(data);
  return env;
}

Envelope parse(std::string_view raw) {
  const auto obj = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
  Envelope env;
  if (obj.is_discarded() || !obj.is_object()) {
    return env;
  }
  env.event_id = obj.value("eventId", "");
  env.event_type = obj.value("eventType", "");
  env.schema_version = obj.value("schemaVersion", kSchemaVersion);
  env.occurred_at = obj.value("occurredAt", "");
  env.saga_id = obj.value("sagaId", "");
  env.idempotency_key = obj.value("idempotencyKey", "");
  env.traceparent = obj.value("traceparent", "");
  env.data = obj.value("data", nlohmann::json::object());
  return env;
}

}  // namespace inventory::event
