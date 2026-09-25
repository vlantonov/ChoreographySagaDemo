// Package config loads Order service configuration from the environment. Defaults
// match the hostnames/ports declared in docs/tech-stack.md so the service runs
// unmodified inside the Compose/Kubernetes stack owned by the Release Engineer.
package config

import (
	"os"
	"strconv"
	"time"
)

type Config struct {
	DatabaseURL    string
	KafkaBrokers   string
	GRPCAddr       string
	HTTPAddr       string
	OTLPEndpoint   string
	ServiceName    string
	ConsumerGroup  string
	RelayInterval  time.Duration
	RelayBatchSize int
}

func Load() Config {
	return Config{
		DatabaseURL:    env("ORDER_DATABASE_URL", "postgres://order:order@orders-db:5432/orders?sslmode=disable"),
		KafkaBrokers:   env("KAFKA_BROKERS", "kafka:9092"),
		GRPCAddr:       env("ORDER_GRPC_ADDR", ":50051"),
		HTTPAddr:       env("ORDER_HTTP_ADDR", ":8080"),
		OTLPEndpoint:   env("OTEL_EXPORTER_OTLP_ENDPOINT", "otel-collector:4317"),
		ServiceName:    env("OTEL_SERVICE_NAME", "order"),
		ConsumerGroup:  env("KAFKA_CONSUMER_GROUP", "order-service"),
		RelayInterval:  envDuration("OUTBOX_RELAY_INTERVAL", 500*time.Millisecond),
		RelayBatchSize: envInt("OUTBOX_RELAY_BATCH", 100),
	}
}

func env(k, def string) string {
	if v, ok := os.LookupEnv(k); ok {
		return v
	}
	return def
}

func envInt(k string, def int) int {
	if v, ok := os.LookupEnv(k); ok {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func envDuration(k string, def time.Duration) time.Duration {
	if v, ok := os.LookupEnv(k); ok {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}
