// Unit tests for the JSON event envelope (tech-stack §5.1).
#include <gtest/gtest.h>

#include "inventory/envelope.hpp"

namespace {

using inventory::event::Envelope;
using inventory::event::kSchemaVersion;

TEST(Envelope, RoundTripsThroughJson) {
  const auto env = inventory::event::make_envelope(
      "InventoryReserved", "saga-1",
      {{"sku", "SKU-1"}, {"quantity", 2}, {"status", "RESERVED"}}, "trace-parent");

  const auto parsed = inventory::event::parse(env.to_json());

  EXPECT_EQ(parsed.event_type, "InventoryReserved");
  EXPECT_EQ(parsed.saga_id, "saga-1");
  EXPECT_EQ(parsed.schema_version, kSchemaVersion);
  EXPECT_EQ(parsed.traceparent, "trace-parent");
  EXPECT_FALSE(parsed.event_id.empty());
  EXPECT_FALSE(parsed.idempotency_key.empty());
  EXPECT_EQ(parsed.data.value("sku", ""), "SKU-1");
  EXPECT_EQ(parsed.data.value("quantity", 0), 2);
}

TEST(Envelope, ParsesTolerantlyOnGarbage) {
  const Envelope env = inventory::event::parse("not json");
  EXPECT_TRUE(env.event_type.empty());
  EXPECT_TRUE(env.saga_id.empty());
}

}  // namespace
