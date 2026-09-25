"""OpenTelemetry + structured JSON logging for the Payment service.

Every log line carries trace_id/span_id for trace-to-log correlation
(tech-stack §9, C-3..C-8, FR-22, FR-26).
"""

from __future__ import annotations

import json
import logging
import sys
from datetime import datetime, timezone

from opentelemetry import metrics, trace
from opentelemetry.exporter.otlp.proto.grpc.metric_exporter import OTLPMetricExporter
from opentelemetry.exporter.otlp.proto.grpc.trace_exporter import OTLPSpanExporter
from opentelemetry.sdk.metrics import MeterProvider
from opentelemetry.sdk.metrics.export import PeriodicExportingMetricReader
from opentelemetry.sdk.resources import Resource
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor
from opentelemetry.trace.propagation.tracecontext import TraceContextTextMapPropagator

_propagator = TraceContextTextMapPropagator()
_METRIC_EXPORT_INTERVAL_MS = 15_000


class _JsonTraceFormatter(logging.Formatter):
    """Formats records as JSON, injecting the active trace_id/span_id."""

    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, object] = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
        }
        span = trace.get_current_span()
        ctx = span.get_span_context()
        if ctx.is_valid:
            payload["trace_id"] = format(ctx.trace_id, "032x")
            payload["span_id"] = format(ctx.span_id, "016x")
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload)


def setup(service_name: str, otlp_endpoint: str) -> tuple[trace.Tracer, metrics.Meter]:
    """Initialise tracing, metrics, and logging; return the tracer and meter."""
    resource = Resource.create({"service.name": service_name})

    tracer_provider = TracerProvider(resource=resource)
    tracer_provider.add_span_processor(
        BatchSpanProcessor(OTLPSpanExporter(endpoint=otlp_endpoint))
    )
    trace.set_tracer_provider(tracer_provider)

    reader = PeriodicExportingMetricReader(
        OTLPMetricExporter(endpoint=otlp_endpoint),
        export_interval_millis=_METRIC_EXPORT_INTERVAL_MS,
    )
    meter_provider = MeterProvider(resource=resource, metric_readers=[reader])
    metrics.set_meter_provider(meter_provider)

    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(_JsonTraceFormatter())
    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(logging.INFO)

    return trace.get_tracer(service_name), metrics.get_meter(service_name)


def inject_traceparent() -> str:
    """Serialize the active span context into a W3C traceparent string."""
    carrier: dict[str, str] = {}
    _propagator.inject(carrier)
    return carrier.get("traceparent", "")


def context_from_traceparent(traceparent: str):
    """Extract an OTel context from a W3C traceparent header (FR-23)."""
    return _propagator.extract({"traceparent": traceparent}) if traceparent else None
