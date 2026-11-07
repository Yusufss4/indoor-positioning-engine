#include <gtest/gtest.h>

#include "ipe/rssi_model.hpp"
#include "ipe/wcl.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

/// Free-space RSSI from every beacon as heard at `pos`, one round every 0.2 s from `t0` to `t1`.
void feed(WeightedCentroid& wcl, const Venue& v, Vec2 pos, double t0, double t1) {
  RssiModelConfig cfg;
  cfg.wall_aware = false;
  const RssiModel model(v, cfg);
  for (double t = t0; t < t1; t += 0.2) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) wcl.on_rssi(t, i, model.expected_rssi(v.beacons()[i], pos));
  }
}

/// The two-room box without the door: the rooms only meet through the wall.
Venue sealed_venue() {
  return parse_venue(R"({
    "name": "sealed", "bounds": [0, 0, 20, 10],
    "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,10]],
    "beacons": [{"id": "A", "x": 2, "y": 2}, {"id": "B", "x": 8, "y": 8},
                {"id": "C", "x": 12, "y": 2}, {"id": "D", "x": 18, "y": 8}]
  })");
}

WeightedCentroid make(const Venue& v, bool walls) {
  WclConfig cfg;
  cfg.window_s = 0.5;
  cfg.top_k = 2;
  cfg.wall_constraint = walls;
  cfg.wall_escape_s = 2.0;
  return WeightedCentroid(v, cfg, {});
}

}  // namespace

TEST(WeightedCentroid, MapFreeByDefault) {
  const Venue v = test::two_room_venue();
  WeightedCentroid wcl = make(v, false);
  feed(wcl, v, {4.0, 4.0}, 0.0, 2.0);
  ASSERT_LT(wcl.estimate().pos.x, 10.0);
  feed(wcl, v, {16.0, 4.0}, 2.0, 3.0);
  EXPECT_GT(wcl.estimate().pos.x, 10.0);
}

TEST(WeightedCentroid, WallConstraintHoldsTheFixUntilTheEscapeTime) {
  const Venue v = sealed_venue();
  WeightedCentroid wcl = make(v, true);
  feed(wcl, v, {4.0, 4.0}, 0.0, 2.0);
  ASSERT_LT(wcl.estimate().pos.x, 10.0);
  // Across the wall from the last fix, the centroid is held on this side for 2 s...
  feed(wcl, v, {16.0, 4.0}, 2.0, 3.5);
  EXPECT_LT(wcl.estimate().pos.x, 10.0);
  // ...and then let through.
  feed(wcl, v, {16.0, 4.0}, 3.5, 6.0);
  EXPECT_GT(wcl.estimate().pos.x, 10.0);
  EXPECT_EQ(wcl.map_follower().escapes(), 1u);
}

TEST(WeightedCentroid, WallConstraintGoesThroughDoors) {
  // With the door, the centroids on the way across reach the other room without a jump.
  const Venue v = test::two_room_venue();
  WeightedCentroid wcl = make(v, true);
  feed(wcl, v, {4.0, 4.0}, 0.0, 2.0);
  feed(wcl, v, {16.0, 4.0}, 2.0, 3.5);
  EXPECT_GT(wcl.estimate().pos.x, 10.0);
  EXPECT_EQ(wcl.map_follower().escapes(), 0u);
}
