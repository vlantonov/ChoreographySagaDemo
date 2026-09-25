#include "inventory/config.hpp"

#include <cstdlib>

namespace inventory::config {

namespace {

std::string env(const char* key, const std::string& fallback) {
  const char* value = std::getenv(key);
  return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

int env_int(const char* key, int fallback) {
  const char* value = std::getenv(key);
  if (value == nullptr || *value == '\0') return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

Config load() {
  Config cfg;
  cfg.database_url = env(
      "INVENTORY_DATABASE_URL",
      "postgresql://inventory:inventory@inventory-db:5432/inventory");
  cfg.kafka_brokers = env("KAFKA_BROKERS", "kafka:9092");
  cfg.consumer_group = env("KAFKA_CONSUMER_GROUP", "inventory-service");
  cfg.grpc_listen_addr = env("GRPC_LISTEN_ADDR", "0.0.0.0:50052");
  cfg.otlp_endpoint = env("OTEL_EXPORTER_OTLP_ENDPOINT", "http://otel-collector:4317");
  cfg.service_name = env("OTEL_SERVICE_NAME", "inventory");
  cfg.relay_interval_ms = env_int("OUTBOX_RELAY_INTERVAL_MS", 500);
  cfg.relay_batch_size = env_int("OUTBOX_RELAY_BATCH", 100);
  return cfg;
}

}  // namespace inventory::config
