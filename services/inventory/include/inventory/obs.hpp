// Structured JSON logging and OpenTelemetry wiring for the Inventory service
// (tech-stack §9, C-3..C-8, FR-22, FR-26).
//
// Structured logging (log_info/log_error) lives in the core library and has no
// OpenTelemetry dependency, so unit tests link it without the OTel SDK. The
// OTLP exporter setup and W3C traceparent helpers (setup/inject_traceparent)
// are implemented only in the server binary. At runtime, setup() installs a
// trace-context provider so every log line carries trace_id/span_id.
#pragma once

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
