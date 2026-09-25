// Domain entities and pure business rules for the Inventory service.
// Header-only and dependency-free so it is unit-testable in isolation
// (ARCHITECTURE §3, §4.3; tech-stack §10).
#pragma once

#include <string_view>

namespace inventory::domain {

// Poison SKU that deterministically forces a reservation failure, exercising
// the full reverse compensation chain (tech-stack §10, SKU-DEADBEEF).
inline constexpr std::string_view kPoisonSku = "SKU-DEADBEEF";

// Outcome of a stock reservation, mirroring the gRPC ReserveStockResponse.Status
// enum in proto/inventory/v1/inventory.proto.
enum class ReserveStatus {
  Unspecified = 0,
  Reserved = 1,
  InsufficientStock = 2,
};

// Reservation lifecycle states persisted in the reservations table.
enum class ReservationState {
  Reserved,
  Released,
};

// is_poison_sku reports whether a SKU is the forced-failure trigger.
[[nodiscard]] inline bool is_poison_sku(std::string_view sku) noexcept {
  return sku == kPoisonSku;
}

[[nodiscard]] inline const char* to_string(ReserveStatus status) noexcept {
  switch (status) {
    case ReserveStatus::Reserved:
      return "RESERVED";
    case ReserveStatus::InsufficientStock:
      return "INSUFFICIENT_STOCK";
    case ReserveStatus::Unspecified:
      return "STATUS_UNSPECIFIED";
  }
  return "STATUS_UNSPECIFIED";
}

}  // namespace inventory::domain
