// Package obs wires OpenTelemetry (traces + metrics) and structured JSON logging
// with trace_id/span_id correlation (tech-stack §9, C-3..C-8, FR-22, FR-26).
package obs

import (
	"context"
	"log/slog"
	"os"
	"time"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetricgrpc"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/propagation"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"go.opentelemetry.io/otel/trace"
)

// Providers bundles the OTel providers so main can shut them down cleanly.
type Providers struct {
	Tracer trace.Tracer
	Meter  metric.Meter
	tp     *sdktrace.TracerProvider
	mp     *sdkmetric.MeterProvider
}

// Setup initialises OTLP/gRPC exporters, the W3C propagator, and a slog logger
// whose records include the active trace_id/span_id (FR-26, AC-6).
func Setup(ctx context.Context, serviceName, otlpEndpoint string) (*Providers, *slog.Logger, error) {
	res, err := resource.New(ctx, resource.WithAttributes(semconv.ServiceName(serviceName)))
	if err != nil {
		return nil, nil, err
	}

	traceExp, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithEndpoint(otlpEndpoint), otlptracegrpc.WithInsecure())
	if err != nil {
		return nil, nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp),
		sdktrace.WithResource(res),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.TraceContext{})

	metricExp, err := otlpmetricgrpc.New(ctx,
		otlpmetricgrpc.WithEndpoint(otlpEndpoint), otlpmetricgrpc.WithInsecure())
	if err != nil {
		return nil, nil, err
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithResource(res),
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp,
			sdkmetric.WithInterval(15*time.Second))),
	)
	otel.SetMeterProvider(mp)

	logger := slog.New(&traceHandler{
		Handler: slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}),
	}).With("service", serviceName)

	return &Providers{
		Tracer: tp.Tracer(serviceName),
		Meter:  mp.Meter(serviceName),
		tp:     tp,
		mp:     mp,
	}, logger, nil
}

// Shutdown flushes and stops the providers.
func (p *Providers) Shutdown(ctx context.Context) {
	if p.tp != nil {
		_ = p.tp.Shutdown(ctx)
	}
	if p.mp != nil {
		_ = p.mp.Shutdown(ctx)
	}
}

// traceHandler injects trace_id/span_id into every log record for trace-to-log
// correlation in Loki/Tempo (tech-stack §9.2).
type traceHandler struct{ slog.Handler }

func (h *traceHandler) Handle(ctx context.Context, r slog.Record) error {
	if sc := trace.SpanContextFromContext(ctx); sc.IsValid() {
		r.AddAttrs(
			slog.String("trace_id", sc.TraceID().String()),
			slog.String("span_id", sc.SpanID().String()),
		)
	}
	return h.Handler.Handle(ctx, r)
}

func (h *traceHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	return &traceHandler{Handler: h.Handler.WithAttrs(attrs)}
}

func (h *traceHandler) WithGroup(name string) slog.Handler {
	return &traceHandler{Handler: h.Handler.WithGroup(name)}
}
