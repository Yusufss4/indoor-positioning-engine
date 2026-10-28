#include <gtest/gtest.h>

#include <cmath>

#include "ipe/rssi_model.hpp"
#include "test_util.hpp"

using namespace ipe;

TEST(RssiModel, PathLossAndInverseAgree) {
  const Venue v = test::two_room_venue();
  const RssiModel m(v);
  const Beacon& b = v.beacons()[0];
  EXPECT_NEAR(m.expected_rssi_free_space(b, 1.0), b.tx_power_dbm, 1e-9);
  EXPECT_NEAR(m.expected_rssi_free_space(b, 10.0), b.tx_power_dbm - 20.0, 1e-9);
  for (double d : {0.7, 2.0, 5.0, 17.0}) {
    EXPECT_NEAR(m.distance_from_rssi(b, m.expected_rssi_free_space(b, d)), d, 1e-9);
  }
}

TEST(RssiModel, WallAttenuationOnlyWhenWallAware) {
  const Venue v = test::two_room_venue();
  const Beacon& a = v.beacons()[0];  // left room
  const Vec2 right_room{14.0, 2.0};
  RssiModelConfig cfg;
  cfg.wall_aware = true;
  const RssiModel aware(v, cfg);
  cfg.wall_aware = false;
  const RssiModel blind(v, cfg);
  EXPECT_NEAR(blind.expected_rssi(a, right_room) - aware.expected_rssi(a, right_room), v.wall_attenuation_db(), 1e-9);
}

TEST(RssiModel, RobustLikelihoodHasFloor) {
  const Venue v = test::two_room_venue();
  const RssiModel m(v);
  EXPECT_GT(m.log_likelihood(0.0), m.log_likelihood(5.0));
  // A 60 dB outlier must not be infinitely unlikely; it saturates at the
  // uniform component instead of dominating the particle weights.
  const double floor = std::log(m.config().outlier_prob / m.config().outlier_range_db);
  EXPECT_NEAR(m.log_likelihood(60.0), floor, 1e-6);
  EXPECT_NEAR(m.log_likelihood(-60.0), floor, 1e-6);
}
