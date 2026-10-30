#include <gtest/gtest.h>

#include <vector>

#include "ipe/engine.hpp"
#include "ipe/particle_filter.hpp"
#include "ipe/rooms.hpp"
#include "ipe/rssi_model.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

RoomEventConfig unthrottled() {
  RoomEventConfig cfg;
  cfg.update_period_s = 0.0;
  return cfg;
}

/// Feeds the per-room probabilities `p` every 0.1 s for t in [t0, t1).
void hold(RoomTracker& tr, const std::vector<double>& p, double t0, double t1) {
  for (int k = 0; t0 + 0.1 * k < t1 - 1e-9; ++k) tr.update(t0 + 0.1 * k, p);
}

Estimate fix_at(Vec2 pos) {
  Estimate e;
  e.pos = pos;
  e.valid = true;
  return e;
}

}  // namespace

TEST(Rooms, EnterFiresOnlyAfterTheDwellTime) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  hold(tr, {1.0, 0.0}, 0.0, 1.4);
  EXPECT_TRUE(tr.pending_events().empty());
  EXPECT_FALSE(tr.inside(0));

  tr.update(1.5, std::vector<double>{1.0, 0.0});
  const auto ev = tr.take_events();
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].room, 0u);
  EXPECT_EQ(ev[0].type, RoomEventType::Enter);
  EXPECT_DOUBLE_EQ(ev[0].t, 1.5);
  EXPECT_DOUBLE_EQ(ev[0].probability, 1.0);
  EXPECT_TRUE(tr.inside(0));
  EXPECT_TRUE(tr.take_events().empty());
}

TEST(Rooms, ShortExcursionProducesNoEvent) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  hold(tr, {1.0, 0.0}, 0.0, 2.0);
  ASSERT_EQ(tr.take_events().size(), 1u);
  hold(tr, {0.1, 0.9}, 2.0, 3.0);  // 1 s across the doorway, shorter than either dwell
  hold(tr, {1.0, 0.0}, 3.0, 6.0);
  EXPECT_TRUE(tr.take_events().empty());
  EXPECT_TRUE(tr.inside(0));
  EXPECT_FALSE(tr.inside(1));
}

TEST(Rooms, HysteresisBetweenEnterAndExitThresholds) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  hold(tr, {0.5, 0.0}, 0.0, 5.0);  // below enter_prob
  EXPECT_FALSE(tr.inside(0));
  hold(tr, {1.0, 0.0}, 5.0, 7.0);
  EXPECT_TRUE(tr.inside(0));
  hold(tr, {0.45, 0.0}, 7.0, 12.0);  // above exit_prob
  EXPECT_TRUE(tr.inside(0));
  EXPECT_EQ(tr.take_events().size(), 1u);
}

TEST(Rooms, ExitPrecedesEnterWhenWalkingThroughADoor) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  hold(tr, {1.0, 0.0}, 0.0, 2.0);
  ASSERT_EQ(tr.take_events().size(), 1u);
  hold(tr, {0.0, 1.0}, 2.0, 4.0);
  const auto ev = tr.take_events();
  ASSERT_EQ(ev.size(), 2u);
  EXPECT_EQ(ev[0].type, RoomEventType::Exit);
  EXPECT_EQ(ev[0].room, 0u);
  EXPECT_EQ(ev[1].type, RoomEventType::Enter);
  EXPECT_EQ(ev[1].room, 1u);
  EXPECT_NEAR(ev[0].t, 3.5, 1e-9);
  EXPECT_DOUBLE_EQ(ev[0].t, ev[1].t);
  std::vector<std::size_t> now;
  tr.current_rooms(now);
  EXPECT_EQ(now, std::vector<std::size_t>{1});
}

TEST(Rooms, LosingTheFixExitsAfterTheDwell) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  for (int k = 0; k <= 20; ++k) tr.update(0.1 * k, fix_at({5.0, 5.0}), nullptr);
  ASSERT_TRUE(tr.inside(0));
  ASSERT_EQ(tr.take_events().size(), 1u);
  const Estimate lost;
  for (int k = 0; k < 15; ++k) tr.update(2.1 + 0.1 * k, lost, nullptr);
  EXPECT_TRUE(tr.inside(0));
  tr.update(3.7, lost, nullptr);
  const auto ev = tr.take_events();
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].type, RoomEventType::Exit);
  EXPECT_FALSE(tr.inside(0));
}

TEST(Rooms, MeanPositionGivesCertainProbabilities) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, unthrottled());
  tr.update(0.0, fix_at({15.0, 5.0}), nullptr);
  ASSERT_EQ(tr.probabilities().size(), 2u);
  EXPECT_DOUBLE_EQ(tr.probabilities()[0], 0.0);
  EXPECT_DOUBLE_EQ(tr.probabilities()[1], 1.0);
}

TEST(Rooms, UpdatesAreThrottled) {
  const Venue v = test::two_room_venue_with_rooms();
  RoomTracker tr(v, RoomEventConfig{});
  EXPECT_TRUE(tr.due(0.0));
  tr.update(0.0, fix_at({5.0, 5.0}), nullptr);
  EXPECT_FALSE(tr.due(0.1));
  tr.update(0.1, fix_at({15.0, 5.0}), nullptr);  // skipped
  EXPECT_DOUBLE_EQ(tr.probabilities()[0], 1.0);
  EXPECT_TRUE(tr.due(0.25));
}

TEST(Rooms, ParticleMassIsTheRoomProbability) {
  const Venue v = test::two_room_venue_with_rooms();
  ParticleFilterConfig cfg;
  cfg.num_particles = 800;
  cfg.seed = 3;
  ParticleFilter pf(v, cfg, {});
  const RssiModel model(v);
  double t = 0.0;
  for (int r = 0; r < 60; ++r) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) {
      pf.on_rssi(t, i, model.expected_rssi(v.beacons()[i], {5.0, 5.0}));
      t += 0.05;
    }
  }
  RoomTracker tr(v, unthrottled());
  tr.update(t, pf.estimate(), &pf);

  double total = 0.0, west = 0.0;
  for (const auto& p : pf.particles()) {
    total += p.weight;
    if (v.rooms()[0].contains(p.pos)) west += p.weight;
  }
  EXPECT_NEAR(tr.probabilities()[0], west / total, 1e-12);
  EXPECT_GT(tr.probabilities()[0], 0.9);
  EXPECT_NEAR(tr.probabilities()[0] + tr.probabilities()[1], 1.0, 1e-9);
}

TEST(Rooms, EngineReportsEventsForVenuesWithRooms) {
  const Venue v = test::two_room_venue_with_rooms();
  EngineConfig cfg;
  cfg.filter = FilterKind::WeightedCentroid;
  PositioningEngine engine(v, cfg);
  ASSERT_NE(engine.room_tracker(), nullptr);
  const RssiModel model(v);
  for (int k = 0; k < 100; ++k) {
    const double t = 0.05 * k;
    const std::size_t b = static_cast<std::size_t>(k) % v.beacons().size();
    engine.on_ble(t, b, model.expected_rssi(v.beacons()[b], {3.0, 5.0}));
  }
  const auto ev = engine.take_room_events();
  ASSERT_FALSE(ev.empty());
  EXPECT_EQ(ev.front().room, 0u);
  EXPECT_EQ(ev.front().type, RoomEventType::Enter);
  EXPECT_EQ(engine.current_rooms(), std::vector<std::size_t>{0});

  const Venue plain = test::two_room_venue();
  PositioningEngine no_rooms(plain, cfg);
  EXPECT_EQ(no_rooms.room_tracker(), nullptr);
  no_rooms.on_ble(0.0, 0, -60.0);
  EXPECT_TRUE(no_rooms.take_room_events().empty());
  EXPECT_TRUE(no_rooms.current_rooms().empty());
}

TEST(Rooms, TruthEventsFollowTheTruePath) {
  const Venue v = test::two_room_venue_with_rooms();
  std::vector<TruthSample> truth;
  for (int k = 0; k <= 160; ++k) truth.push_back({0.1 * k, {2.0 + 0.1 * k, 5.0}, 0.0, 0, 0.0});
  const auto ev = truth_room_events(v, truth);
  ASSERT_EQ(ev.size(), 3u);
  EXPECT_EQ(ev[0].type, RoomEventType::Enter);
  EXPECT_EQ(ev[0].room, 0u);
  EXPECT_DOUBLE_EQ(ev[0].t, 0.0);
  EXPECT_EQ(ev[1].type, RoomEventType::Exit);
  EXPECT_EQ(ev[2].type, RoomEventType::Enter);
  EXPECT_EQ(ev[2].room, 1u);
  EXPECT_NEAR(ev[2].t, 8.0, 0.11);  // crossing x = 10
}
