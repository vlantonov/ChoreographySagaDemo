// Package grpcapi exposes OrderService over gRPC (and REST via grpc-gateway in
// main). It adapts protobuf messages to the application layer (tech-stack §11).
package grpcapi

import (
	"context"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/vladiant/choreography-saga/order/internal/app"
	orderv1 "github.com/vladiant/choreography-saga/order/internal/pb/order/v1"
	"github.com/vladiant/choreography-saga/order/internal/store"
)

// Server implements the generated OrderServiceServer.
type Server struct {
	orderv1.UnimplementedOrderServiceServer
	app *app.Service
}

func NewServer(a *app.Service) *Server { return &Server{app: a} }

func (s *Server) CreateOrder(ctx context.Context, req *orderv1.CreateOrderRequest) (*orderv1.CreateOrderResponse, error) {
	if req.GetSku() == "" || req.GetQuantity() <= 0 {
		return nil, status.Error(codes.InvalidArgument, "sku and positive quantity are required")
	}
	o, err := s.app.CreateOrder(ctx, req.GetCustomerId(), req.GetSku(), req.GetQuantity(), req.GetAmount())
	if err != nil {
		return nil, status.Errorf(codes.Internal, "create order: %v", err)
	}
	return &orderv1.CreateOrderResponse{OrderId: o.ID, Status: string(o.Status)}, nil
}

func (s *Server) GetOrder(ctx context.Context, req *orderv1.GetOrderRequest) (*orderv1.GetOrderResponse, error) {
	o, err := s.app.GetOrder(ctx, req.GetOrderId())
	if err == store.ErrNotFound {
		return nil, status.Error(codes.NotFound, "order not found")
	}
	if err != nil {
		return nil, status.Errorf(codes.Internal, "get order: %v", err)
	}
	return &orderv1.GetOrderResponse{
		OrderId:  o.ID,
		Status:   string(o.Status),
		Sku:      o.SKU,
		Quantity: o.Quantity,
		Amount:   o.Amount,
	}, nil
}
