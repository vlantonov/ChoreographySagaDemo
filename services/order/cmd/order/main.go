// Command order is the entry point for the Order service (Go). It wires the gRPC
// + REST API, the Kafka consumer that drives the saga state machine, and the
// transactional-outbox relay (ARCHITECTURE §3).
package main

import (
	"context"
	"errors"
	"net"
	"net/http"
	"os/signal"
	"syscall"
	"time"

	"github.com/grpc-ecosystem/grpc-gateway/v2/runtime"
	"github.com/jackc/pgx/v5/pgxpool"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	"github.com/vladiant/choreography-saga/order/internal/app"
	"github.com/vladiant/choreography-saga/order/internal/config"
	"github.com/vladiant/choreography-saga/order/internal/grpcapi"
	"github.com/vladiant/choreography-saga/order/internal/kafkax"
	"github.com/vladiant/choreography-saga/order/internal/obs"
	"github.com/vladiant/choreography-saga/order/internal/outbox"
	orderv1 "github.com/vladiant/choreography-saga/order/internal/pb/order/v1"
	"github.com/vladiant/choreography-saga/order/internal/store"
)

func main() {
	if err := run(); err != nil {
		panic(err)
	}
}

func run() error {
	cfg := config.Load()
	rootCtx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	providers, log, err := obs.Setup(rootCtx, cfg.ServiceName, cfg.OTLPEndpoint)
	if err != nil {
		return err
	}
	defer providers.Shutdown(context.Background())

	pool, err := pgxpool.New(rootCtx, cfg.DatabaseURL)
	if err != nil {
		return err
	}
	defer pool.Close()

	st := store.New(pool)

	producer, err := kafkax.NewProducer(cfg.KafkaBrokers)
	if err != nil {
		return err
	}
	defer producer.Close()

	svc, err := app.New(st, log, providers.Tracer, providers.Meter)
	if err != nil {
		return err
	}

	// Outbox relay (FR-10, FR-11).
	relay, err := outbox.NewRelay(st, producer, log, providers.Meter, cfg.RelayInterval, cfg.RelayBatchSize)
	if err != nil {
		return err
	}
	go relay.Run(rootCtx)

	// Kafka consumer driving the saga state machine.
	consumer, err := kafkax.NewConsumer(cfg.KafkaBrokers, cfg.ConsumerGroup, app.ConsumedTopics())
	if err != nil {
		return err
	}
	defer consumer.Close()
	go consumer.Run(rootCtx, svc.HandleEvent)

	// gRPC server.
	grpcServer := grpc.NewServer()
	orderv1.RegisterOrderServiceServer(grpcServer, grpcapi.NewServer(svc))
	lis, err := net.Listen("tcp", cfg.GRPCAddr)
	if err != nil {
		return err
	}
	go func() {
		log.Info("gRPC listening", "addr", cfg.GRPCAddr)
		if serveErr := grpcServer.Serve(lis); serveErr != nil {
			log.Error("grpc serve", "error", serveErr)
		}
	}()

	// REST gateway (grpc-gateway) over the same handlers (tech-stack §11).
	gwMux := runtime.NewServeMux()
	if err := orderv1.RegisterOrderServiceHandlerFromEndpoint(rootCtx, gwMux, cfg.GRPCAddr,
		[]grpc.DialOption{grpc.WithTransportCredentials(insecure.NewCredentials())}); err != nil {
		return err
	}
	httpSrv := &http.Server{Addr: cfg.HTTPAddr, Handler: gwMux, ReadHeaderTimeout: 5 * time.Second}
	go func() {
		log.Info("REST gateway listening", "addr", cfg.HTTPAddr)
		if serveErr := httpSrv.ListenAndServe(); serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			log.Error("http serve", "error", serveErr)
		}
	}()

	<-rootCtx.Done()
	log.Info("shutting down")

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(shutdownCtx)
	grpcServer.GracefulStop()
	return nil
}
