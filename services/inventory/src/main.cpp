// Entry point for the Inventory service (C++). Wires observability, the libpqxx
// repository, the gRPC ReserveStock/ReleaseStock server, the Kafka consumer for
// order.cancelled compensation, and the transactional-outbox relay
// (ARCHITECTURE §3).
#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "inventory/config.hpp"
#include "inventory/db.hpp"
#include "inventory/envelope.hpp"
#include "inventory/grpc_server.hpp"
#include "inventory/kafka_io.hpp"
#include "inventory/obs.hpp"
#include "inventory/outbox.hpp"
#include "inventory/service.hpp"

namespace {

std::atomic<bool> g_stop{false};
inventory::kafka::Consumer* g_consumer = nullptr;

void handle_signal(int /*signum*/) {
  g_stop.store(true);
  if (g_consumer != nullptr) g_consumer->stop();
}

}  // namespace

int main() {
  const inventory::config::Config cfg = inventory::config::load();
  inventory::obs::setup(cfg.service_name, cfg.otlp_endpoint);
  inventory::obs::log_info("inventory service starting",
                           {{"brokers", cfg.kafka_brokers},
                            {"grpc", cfg.grpc_listen_addr}});

  inventory::db::Database db(cfg.database_url);
  inventory::kafka::Producer producer(cfg.kafka_brokers);
  inventory::app::InventoryApp app(db);
  inventory::outbox::Relay relay(db, producer, cfg.relay_batch_size);

  // gRPC server (the synchronous reservation leg).
  inventory::grpcapi::InventoryServiceImpl service(app);
  grpc::ServerBuilder builder;
  builder.AddListeningPort(cfg.grpc_listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  inventory::obs::log_info("gRPC server listening", {{"addr", cfg.grpc_listen_addr}});

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  // Outbox relay: publish PENDING rows at-least-once (FR-10).
  std::thread relay_thread([&] {
    while (!g_stop.load()) {
      try {
        relay.drain_once();
      } catch (const std::exception& ex) {
        inventory::obs::log_error("outbox relay drain failed", {{"error", ex.what()}});
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(cfg.relay_interval_ms));
    }
  });

  // Kafka consumer: order.cancelled compensation (idempotent stock release).
  inventory::kafka::Consumer consumer(cfg.kafka_brokers, cfg.consumer_group,
                                      inventory::app::InventoryApp::consumed_topics());
  g_consumer = &consumer;
  std::thread consumer_thread([&] {
    consumer.run([&](const std::string& topic, const std::string& value,
                     const std::string& traceparent) {
      if (topic != inventory::app::kTopicOrderCancelled) return;
      inventory::obs::SpanScope span = inventory::obs::start_span("consume order.cancelled",
                                                                  traceparent);
      const inventory::event::Envelope env = inventory::event::parse(value);
      app.handle_order_cancelled(env, traceparent);
    });
  });

  consumer_thread.join();  // blocks until stop()
  server->Shutdown();
  g_stop.store(true);
  relay_thread.join();
  inventory::obs::shutdown();
  inventory::obs::log_info("inventory service stopped", {});
  return 0;
}
