// Kafka producer/consumer wrappers (librdkafka C++ RdKafka API) with manual
// offset commit and W3C traceparent propagation via headers (tech-stack §3,
// FR-23).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "inventory/outbox.hpp"

namespace RdKafka {
class Producer;
class KafkaConsumer;
}  // namespace RdKafka

namespace inventory::kafka {

inline constexpr const char* kTraceparentHeader = "traceparent";

// Producer implements the outbox Publisher over librdkafka.
class Producer : public outbox::Publisher {
 public:
  explicit Producer(const std::string& brokers);
  ~Producer() override;

  void publish(const std::string& topic, const std::string& key,
               const std::string& payload, const std::string& traceparent) override;

 private:
  std::unique_ptr<RdKafka::Producer> producer_;
};

// Handler receives (topic, value, traceparent) and throws on failure so the
// message is redelivered (idempotency absorbs the duplicate).
using Handler = std::function<void(const std::string& topic, const std::string& value,
                                   const std::string& traceparent)>;

// Consumer subscribes to topics and commits offsets only after the handler
// succeeds (at-least-once, tech-stack §3).
class Consumer {
 public:
  Consumer(const std::string& brokers, const std::string& group,
           const std::vector<std::string>& topics);
  ~Consumer();

  void run(const Handler& handler);
  void stop();

 private:
  std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
  bool running_ = false;
};

}  // namespace inventory::kafka
