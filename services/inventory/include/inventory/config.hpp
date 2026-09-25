// Inventory service configuration from environment variables. Defaults match the
// hostnames/ports in docs/tech-stack.md so the service runs unmodified inside
// the Release Engineer's stack.
#pragma once

#include <string>

namespace inventory::config {

struct Config {
  std::string database_url;       // libpqxx connection string
  std::string kafka_brokers;
  std::string consumer_group;
  std::string grpc_listen_addr;
  std::string otlp_endpoint;
  std::string service_name;
  int relay_interval_ms = 500;
  int relay_batch_size = 100;
};

// load reads configuration from the process environment.
[[nodiscard]] Config load();

}  // namespace inventory::config
