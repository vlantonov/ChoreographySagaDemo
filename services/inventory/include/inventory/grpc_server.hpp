// gRPC server adapter for InventoryService (tech-stack §6, ARCHITECTURE §7).
// Translates the proto contract to the application layer and continues the
// distributed trace from incoming W3C traceparent metadata (FR-23).
#pragma once

#include "inventory/v1/inventory.grpc.pb.h"
#include "inventory/service.hpp"

namespace inventory::grpcapi {

class InventoryServiceImpl final : public ::inventory::v1::InventoryService::Service {
 public:
  explicit InventoryServiceImpl(app::InventoryApp& application);

  ::grpc::Status ReserveStock(::grpc::ServerContext* context,
                              const ::inventory::v1::ReserveStockRequest* request,
                              ::inventory::v1::ReserveStockResponse* response) override;

  ::grpc::Status ReleaseStock(::grpc::ServerContext* context,
                              const ::inventory::v1::ReleaseStockRequest* request,
                              ::inventory::v1::ReleaseStockResponse* response) override;

 private:
  app::InventoryApp& app_;
};

}  // namespace inventory::grpcapi
