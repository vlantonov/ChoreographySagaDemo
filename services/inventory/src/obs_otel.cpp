// OpenTelemetry C++ SDK wiring for the Inventory service (tech-stack §9): OTLP
// trace + metric export, W3C traceparent propagation, and the trace-context
// provider that stamps trace_id/span_id onto every structured log line
// (FR-22, FR-23, FR-26). Compiled only into the server binary.
#include "inventory/obs.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <opentelemetry/context/propagation/global_propagator.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/view/view_registry.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span.h>

namespace otel = opentelemetry;

namespace inventory::obs {

namespace {

otel::nostd::shared_ptr<otel::trace::Tracer> g_tracer;
otel::nostd::shared_ptr<otel::metrics::Counter<uint64_t>> g_reserve_counter;
std::shared_ptr<otel::context::propagation::TextMapPropagator> g_propagator;

// MapCarrier adapts a string map to the OTel text-map carrier for W3C
// traceparent inject/extract.
class MapCarrier : public otel::context::propagation::TextMapCarrier {
 public:
  std::map<std::string, std::string> data;

  otel::nostd::string_view Get(otel::nostd::string_view key) const noexcept override {
    const auto it = data.find(std::string(key));
    return it == data.end() ? otel::nostd::string_view{} : otel::nostd::string_view{it->second};
  }
  void Set(otel::nostd::string_view key, otel::nostd::string_view value) noexcept override {
    data[std::string(key)] = std::string(value);
  }
};

void install_log_trace_context() {
  set_trace_context_provider([](nlohmann::json& obj) {
    const auto ctx = otel::context::RuntimeContext::GetCurrent();
    const auto span = otel::trace::GetSpan(ctx);
    const auto sc = span->GetContext();
    if (!sc.IsValid()) return;
    std::array<char, 32> trace_id{};
    std::array<char, 16> span_id{};
    sc.trace_id().ToLowerBase16(trace_id);
    sc.span_id().ToLowerBase16(span_id);
    obj["trace_id"] = std::string(trace_id.data(), trace_id.size());
    obj["span_id"] = std::string(span_id.data(), span_id.size());
  });
}

}  // namespace

void setup(const std::string& service_name, const std::string& otlp_endpoint) {
  auto resource = otel::sdk::resource::Resource::Create(
      {{"service.name", service_name}});

  // --- traces ---
  otel::exporter::otlp::OtlpGrpcExporterOptions trace_opts;
  trace_opts.endpoint = otlp_endpoint;
  auto trace_exporter = otel::exporter::otlp::OtlpGrpcExporterFactory::Create(trace_opts);
  otel::sdk::trace::BatchSpanProcessorOptions bsp_opts;
  auto processor = otel::sdk::trace::BatchSpanProcessorFactory::Create(
      std::move(trace_exporter), bsp_opts);
  std::shared_ptr<otel::trace::TracerProvider> tracer_provider =
      otel::sdk::trace::TracerProviderFactory::Create(std::move(processor), resource);
  otel::trace::Provider::SetTracerProvider(tracer_provider);
  g_tracer = tracer_provider->GetTracer(service_name);

  // --- metrics ---
  otel::exporter::otlp::OtlpGrpcMetricExporterOptions metric_opts;
  metric_opts.endpoint = otlp_endpoint;
  auto metric_exporter =
      otel::exporter::otlp::OtlpGrpcMetricExporterFactory::Create(metric_opts);
  otel::sdk::metrics::PeriodicExportingMetricReaderOptions reader_opts;
  reader_opts.export_interval_millis = std::chrono::milliseconds(15000);
  auto reader = otel::sdk::metrics::PeriodicExportingMetricReaderFactory::Create(
      std::move(metric_exporter), reader_opts);
  auto meter_provider = otel::sdk::metrics::MeterProviderFactory::Create(
      std::make_unique<otel::sdk::metrics::ViewRegistry>(), resource);
  meter_provider->AddMetricReader(std::move(reader));
  std::shared_ptr<otel::metrics::MeterProvider> shared_meter_provider =
      std::move(meter_provider);
  otel::metrics::Provider::SetMeterProvider(shared_meter_provider);
  auto meter = shared_meter_provider->GetMeter(service_name);
  g_reserve_counter =
      meter->CreateUInt64Counter("inventory_reserve_total", "reserve outcomes");

  // --- propagation + log correlation ---
  g_propagator = std::make_shared<otel::trace::propagation::HttpTraceContext>();
  otel::context::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
      otel::nostd::shared_ptr<otel::context::propagation::TextMapPropagator>(
          new otel::trace::propagation::HttpTraceContext()));
  install_log_trace_context();
}

void shutdown() {
  const auto provider = otel::trace::Provider::GetTracerProvider();
  if (auto* sdk = dynamic_cast<otel::sdk::trace::TracerProvider*>(provider.get())) {
    sdk->ForceFlush();
  }
}

std::string inject_traceparent() {
  if (!g_propagator) return {};
  MapCarrier carrier;
  g_propagator->Inject(carrier, otel::context::RuntimeContext::GetCurrent());
  const auto it = carrier.data.find("traceparent");
  return it == carrier.data.end() ? std::string{} : it->second;
}

void record_reserve(std::string_view result) {
  if (g_reserve_counter) {
    g_reserve_counter->Add(1, {{"result", std::string(result)}});
  }
}

struct SpanScope::Impl {
  otel::nostd::shared_ptr<otel::trace::Span> span;
  std::unique_ptr<otel::trace::Scope> scope;
};

SpanScope::SpanScope() = default;

SpanScope::~SpanScope() {
  if (impl_ != nullptr) {
    if (impl_->span) impl_->span->End();
    delete impl_;
  }
}

SpanScope::SpanScope(SpanScope&& other) noexcept : impl_(other.impl_) {
  other.impl_ = nullptr;
}

SpanScope& SpanScope::operator=(SpanScope&& other) noexcept {
  if (this != &other) {
    if (impl_ != nullptr) {
      if (impl_->span) impl_->span->End();
      delete impl_;
    }
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

SpanScope start_span(std::string_view name, const std::string& traceparent) {
  SpanScope holder;
  if (!g_tracer) return holder;

  otel::trace::StartSpanOptions opts;
  if (g_propagator && !traceparent.empty()) {
    MapCarrier carrier;
    carrier.data["traceparent"] = traceparent;
    const auto ctx = g_propagator->Extract(carrier, otel::context::RuntimeContext::GetCurrent());
    opts.parent = otel::trace::GetSpan(ctx)->GetContext();
  }
  holder.impl_ = new SpanScope::Impl();
  holder.impl_->span = g_tracer->StartSpan(std::string(name), opts);
  holder.impl_->scope =
      std::make_unique<otel::trace::Scope>(g_tracer->WithActiveSpan(holder.impl_->span));
  return holder;
}

}  // namespace inventory::obs
