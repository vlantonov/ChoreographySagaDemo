// Unit tests for the Inventory application layer (ARCHITECTURE §7, §8):
// reserve success decrements stock, poison-SKU reservation failure, release
// restores stock, and idempotent dedupe of a repeated request.
#include <gtest/gtest.h>

#include "fakes.hpp"
#include "inventory/service.hpp"

namespace {

using inventory::app::InventoryApp;
using inventory::domain::ReserveStatus;
using inventory::testing::FakeRepository;

FakeRepository make_repo() { return FakeRepository({{"SKU-1", 5}, {"SKU-DEADBEEF", 0}}); }

TEST(InventoryApp, ReserveSuccessDecrementsStock) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);

  const auto result = app.reserve_stock("saga-1", "idem-1", "SKU-1", 2, "tp");

  EXPECT_EQ(result.status, ReserveStatus::Reserved);
  EXPECT_FALSE(result.reservation_id.empty());
  EXPECT_EQ(repo.available["SKU-1"], 3);
  EXPECT_EQ(repo.reserved_units["SKU-1"], 2);
  ASSERT_EQ(repo.emitted.size(), 1U);
  EXPECT_EQ(repo.emitted[0].topic, inventory::app::kTopicInventoryReserved);
  EXPECT_EQ(repo.emitted[0].event_type, "InventoryReserved");
}

TEST(InventoryApp, PoisonSkuReservationFails) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);

  const auto result = app.reserve_stock("saga-2", "idem-2", "SKU-DEADBEEF", 1, "tp");

  EXPECT_EQ(result.status, ReserveStatus::InsufficientStock);
  EXPECT_TRUE(result.reservation_id.empty());
  EXPECT_EQ(repo.available["SKU-DEADBEEF"], 0);
  EXPECT_TRUE(repo.reservations.find("saga-2") == repo.reservations.end());
  ASSERT_EQ(repo.emitted.size(), 1U);
  EXPECT_EQ(repo.emitted[0].topic, inventory::app::kTopicInventoryReservationFailed);
  EXPECT_EQ(repo.emitted[0].event_type, "InventoryReservationFailed");
}

TEST(InventoryApp, InsufficientStockReservationFails) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);

  const auto result = app.reserve_stock("saga-3", "idem-3", "SKU-1", 99, "tp");

  EXPECT_EQ(result.status, ReserveStatus::InsufficientStock);
  EXPECT_EQ(repo.available["SKU-1"], 5);  // unchanged
  ASSERT_EQ(repo.emitted.size(), 1U);
  EXPECT_EQ(repo.emitted[0].topic, inventory::app::kTopicInventoryReservationFailed);
}

TEST(InventoryApp, ReleaseRestoresStock) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);
  ASSERT_EQ(app.reserve_stock("saga-4", "idem-4", "SKU-1", 2, "tp").status,
            ReserveStatus::Reserved);
  ASSERT_EQ(repo.available["SKU-1"], 3);

  const bool released = app.release_stock("saga-4", "", "idem-4-rel", "tp");

  EXPECT_TRUE(released);
  EXPECT_EQ(repo.available["SKU-1"], 5);  // restored
  EXPECT_EQ(repo.reserved_units["SKU-1"], 0);
  ASSERT_EQ(repo.emitted.size(), 2U);
  EXPECT_EQ(repo.emitted[1].topic, inventory::app::kTopicInventoryReleased);
  EXPECT_EQ(repo.emitted[1].event_type, "InventoryReleased");
}

TEST(InventoryApp, RepeatedReserveIsDeduped) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);

  const auto first = app.reserve_stock("saga-5", "idem-5", "SKU-1", 2, "tp");
  const auto second = app.reserve_stock("saga-5", "idem-5", "SKU-1", 2, "tp");

  EXPECT_EQ(first.status, ReserveStatus::Reserved);
  EXPECT_EQ(second.status, ReserveStatus::Reserved);
  EXPECT_EQ(second.reservation_id, first.reservation_id);
  EXPECT_EQ(repo.available["SKU-1"], 3);  // decremented exactly once
  EXPECT_EQ(repo.emitted.size(), 1U);     // event emitted exactly once
}

TEST(InventoryApp, RepeatedReleaseIsDeduped) {
  FakeRepository repo = make_repo();
  InventoryApp app(repo);
  ASSERT_EQ(app.reserve_stock("saga-6", "idem-6", "SKU-1", 2, "tp").status,
            ReserveStatus::Reserved);

  EXPECT_TRUE(app.release_stock("saga-6", "", "rel-6", "tp"));
  EXPECT_FALSE(app.release_stock("saga-6", "", "rel-6", "tp"));  // duplicate

  EXPECT_EQ(repo.available["SKU-1"], 5);  // restored exactly once
}

}  // namespace
