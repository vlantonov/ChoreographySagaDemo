// Unit tests for the transactional-outbox relay (tech-stack §8.2, AC-8).
#include <gtest/gtest.h>

#include "fakes.hpp"
#include "inventory/outbox.hpp"

namespace {

using inventory::outbox::Record;
using inventory::outbox::Relay;
using inventory::testing::FakeOutboxRepo;
using inventory::testing::FakePublisher;

TEST(Relay, DrainPublishesAndMarksPending) {
  FakeOutboxRepo repo;
  repo.pending = {
      Record{"1", "inventory.reserved", "saga-1", "{}", "tp-1"},
      Record{"2", "inventory.reservation_failed", "saga-2", "{}", "tp-2"},
  };
  FakePublisher pub;
  Relay relay(repo, pub, 100);

  const int published = relay.drain_once();

  EXPECT_EQ(published, 2);
  EXPECT_EQ(pub.sent.size(), 2U);
  EXPECT_EQ(pub.sent[0].topic, "inventory.reserved");
  EXPECT_EQ(pub.sent[0].traceparent, "tp-1");
  EXPECT_EQ(repo.published.size(), 2U);
}

TEST(Relay, PublishFailureLeavesRowPending) {
  FakeOutboxRepo repo;
  repo.pending = {Record{"1", "inventory.reserved", "saga-1", "{}", "tp-1"}};
  FakePublisher pub;
  pub.fail_next = true;
  Relay relay(repo, pub, 100);

  EXPECT_THROW(relay.drain_once(), std::runtime_error);
  EXPECT_TRUE(repo.published.empty());   // not marked published
  EXPECT_EQ(repo.failed["1"], 1);        // attempt counter bumped
}

TEST(Relay, PendingCountTracksUnpublishedRows) {
  FakeOutboxRepo repo;
  repo.pending = {
      Record{"1", "inventory.reserved", "saga-1", "{}", "tp-1"},
      Record{"2", "inventory.reservation_failed", "saga-2", "{}", "tp-2"},
  };
  FakePublisher pub;
  Relay relay(repo, pub, 100);

  EXPECT_EQ(relay.pending_count(), 2);   // backlog gauge before draining
  EXPECT_EQ(relay.drain_once(), 2);
  EXPECT_EQ(relay.pending_count(), 0);   // all rows published
}

}  // namespace
