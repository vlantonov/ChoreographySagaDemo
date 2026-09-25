#include "inventory/grpc_server.hpp"

#include <string>

#include "inventory/obs.hpp"

namespace inventory::grpcapi {

namespace {

// traceparent_of extracts the W3C traceparent from incoming gRPC metadata.
std::string traceparent_of(const ::grpc::ServerContext* context) {
  const auto& md = context->client_metadata();
  const auto it = md.find("traceparent");
  if (it == md.end()) return {};
  return std::string(it->second.data(), it->second.size());
}

::inventory::v1::ReserveStockResponse::Status to_proto(domain::ReserveStatus status) {
  switch (status) {
    case domain::ReserveStatus::Reserved:
      return ::inventory::v1::ReserveStockResponse::RESERVED;
    case domain::ReserveStatus::InsufficientStock:
      return ::inventory::v1::ReserveStockResponse::INSUFFICIENT_STOCK;
    case domain::ReserveStatus::Unspecified:
      return ::inventory::v1::ReserveStockResponse::STATUS_UNSPECIFIED;
  }
  return ::inventory::v1::ReserveStockResponse::STATUS_UNSPECIFIED;
}

}  // namespace

InventoryServiceImpl::InventoryServiceImpl(app::InventoryApp& application)
    : app_(application) {}

::grpc::Status InventoryServiceImpl::ReserveStock(
    ::grpc::ServerContext* context, const ::inventory::v1::ReserveStockRequest* request,
    ::inventory::v1::ReserveStockResponse* response) {
  const std::string traceparent = traceparent_of(context);
  const auto in_tp = traceparent.empty() ? obs::inject_traceparent() : traceparent;
  obs::SpanScope span = obs::start_span("ReserveStock", in_tp);

  const app::ReserveResult result = app_.reserve_stock(
      request->saga_id(), request->idempotency_key(), request->sku(),
      request->quantity(), obs::inject_traceparent());

  response->set_status(to_proto(result.status));
  response->set_reservation_id(result.reservation_id);
  response->set_message(result.message);
  obs::record_reserve(domain::to_string(result.status));
  return ::grpc::Status::OK;
}

::grpc::Status InventoryServiceImpl::ReleaseStock(
    ::grpc::ServerContext* context, const ::inventory::v1::ReleaseStockRequest* request,
    ::inventory::v1::ReleaseStockResponse* response) {
  const std::string traceparent = traceparent_of(context);
  const auto in_tp = traceparent.empty() ? obs::inject_traceparent() : traceparent;
  obs::SpanScope span = obs::start_span("ReleaseStock", in_tp);

  const bool released = app_.release_stock(
      request->saga_id(), request->reservation_id(), request->idempotency_key(),
      obs::inject_traceparent());

  response->set_released(released);
  return ::grpc::Status::OK;
}

}  // namespace inventory::grpcapi
