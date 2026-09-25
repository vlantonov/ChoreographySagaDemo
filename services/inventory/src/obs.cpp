// Core structured logging (no OpenTelemetry dependency) so the unit-test binary
// links without the OTel SDK. The OTLP/tracing implementation lives in
// obs_otel.cpp, compiled only into the server binary.
#include "inventory/obs.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>

namespace inventory::obs {

namespace {

std::mutex g_log_mutex;
TraceContextFn g_trace_context;  // default: empty (no-op)

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

void emit(std::string_view level, std::string_view message,
          std::initializer_list<Field> fields) {
  nlohmann::json obj{
      {"timestamp", timestamp()},
      {"level", level},
      {"logger", "inventory"},
      {"message", std::string(message)},
  };
  for (const Field& f : fields) {
    obj[f.key] = f.value;
  }
  if (g_trace_context) {
    g_trace_context(obj);
  }
  const std::lock_guard<std::mutex> lock(g_log_mutex);
  std::cout << obj.dump() << '\n';
  std::cout.flush();
}

}  // namespace

void log_info(std::string_view message, std::initializer_list<Field> fields) {
  emit("INFO", message, fields);
}

void log_error(std::string_view message, std::initializer_list<Field> fields) {
  emit("ERROR", message, fields);
}

void set_trace_context_provider(TraceContextFn fn) { g_trace_context = std::move(fn); }

}  // namespace inventory::obs
