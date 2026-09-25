// Structured JSON logging and OpenTelemetry wiring for the Inventory service
// (tech-stack §9, C-3..C-8, FR-22, FR-26).
//
// Structured logging (log_info/log_error) lives in the core library and has no
// OpenTelemetry dependency, so unit tests link it without the OTel SDK. The
// OTLP exporter setup and W3C traceparent helpers (setup/inject_traceparent)
// are implemented only in the server binary. At runtime, setup() installs a
// trace-context provider so every log line carries trace_id/span_id.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace inventory::obs {

// Field is a structured key/value pair attached to a log line.
struct Field {
  std::string key;
  nlohmann::json value;
};

// log_info / log_error emit one structured JSON line to stdout, enriched with
// the active trace_id/span_id when a provider is installed.
void log_info(std::string_view message, std::initializer_list<Field> fields = {});
void log_error(std::string_view message, std::initializer_list<Field> fields = {});

// TraceContextFn enriches a log object with trace correlation fields.
using TraceContextFn = std::function<void(nlohmann::json&)>;

// set_trace_context_provider installs the hook used to add trace_id/span_id to
// each log line. Called by setup(); defaults to a no-op for tests.
void set_trace_context_provider(TraceContextFn fn);

// --- outbox SLO metrics (tech-stack §9.3), core-side hook ---

// OutboxLagRecorder receives the age (seconds) of an outbox row at publish
// time. setup() installs an OpenTelemetry-backed recorder; the core library and
// tests default to a no-op, keeping the relay free of an OTel dependency.
using OutboxLagRecorder = std::function<void(double)>;

// set_outbox_lag_recorder installs the publish-lag recorder. setup() calls it
// with the OTel histogram; tests may install a capturing recorder.
void set_outbox_lag_recorder(OutboxLagRecorder fn);

// record_outbox_publish_lag records one outbox_publish_lag_seconds sample via
// the installed recorder (no-op if none is installed).
void record_outbox_publish_lag(double seconds);

// --- server-only (implemented against the OpenTelemetry C++ SDK) ---

// setup initialises the tracer/meter providers, the OTLP exporter, and the log
// trace-context provider.
void setup(const std::string& service_name, const std::string& otlp_endpoint);

// shutdown flushes exporters before process exit.
void shutdown();

// inject_traceparent serialises the active span context into a W3C traceparent.
std::string inject_traceparent();

// record_reserve increments the reserve-outcome counter (tech-stack §9.3).
void record_reserve(std::string_view result);

// record_reserve_duration records the ReserveStock server-handler latency
// (seconds) into the reserve_stock_server_duration_seconds histogram
// (tech-stack §9.3, gRPC ReserveStock p95 SLO).
void record_reserve_duration(double seconds, std::string_view result);

// register_outbox_pending_gauge installs an observable gauge (outbox_pending)
// that reports the current backlog via observe() on each metric collection.
void register_outbox_pending_gauge(std::function<int64_t()> observe);

// start_span begins a span for the given operation and makes it current for the
// lifetime of the returned scope holder (opaque, server-only).
class SpanScope {
 public:
  SpanScope();
  ~SpanScope();
  SpanScope(SpanScope&&) noexcept;
  SpanScope& operator=(SpanScope&&) noexcept;
  SpanScope(const SpanScope&) = delete;
  SpanScope& operator=(const SpanScope&) = delete;

 private:
  friend SpanScope start_span(std::string_view name, const std::string& traceparent);
  struct Impl;
  Impl* impl_ = nullptr;
};

// start_span continues the trace from an incoming W3C traceparent (empty starts
// a new root) and returns a scope that ends the span on destruction.
SpanScope start_span(std::string_view name, const std::string& traceparent);

}  // namespace inventory::obs
