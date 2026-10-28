#include <gtest/gtest.h>

#include <algorithm>

#include "ipe/baro.hpp"
#include "ipe/particle_filter.hpp"
#include "ipe/rssi_model.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

ParticleFilterConfig test_config() {
  ParticleFilterConfig cfg;
  cfg.num_particles = 800;
  cfg.seed = 11;
  cfg.recovery = false;
  cfg.estimate_rssi_offset = false;
  return cfg;
}

/// Feeds noiseless, wall-aware RSSI from every beacon as seen from `pos`.
void feed_rssi(ParticleFilter& pf, const Venue& v, Vec2 pos, int rounds, double t0 = 0.0, double bias_a = 0.0) {
  const RssiModel model(v);
  double t = t0;
  for (int r = 0; r < rounds; ++r) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) {
      const double bias = i == 0 ? bias_a : 0.0;
      pf.on_rssi(t, i, model.expected_rssi(v.beacons()[i], pos) + bias);
      t += 0.05;
    }
  }
}

void feed_rssi_on_level(ParticleFilter& pf, const Venue& v, Vec2 pos, int level, int rounds, double t0 = 0.0) {
  const RssiModel model(v);
  double t = t0;
  for (int r = 0; r < rounds; ++r) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) {
      pf.on_rssi(t, i, model.expected_rssi(v.beacons()[i], pos, level));
      t += 0.05;
    }
  }
}

/// Holds the pressure for a while, then ramps it as if the phone rose `rise_m`.
void feed_rise(ParticleFilter& pf, double t0, double rise_m) {
  const double p0 = 1000.0;
  const double alt0 = pressure_to_altitude(p0);
  for (int k = 0; k <= 100; ++k) {
    const double t = t0 + 0.2 * k;
    const double frac = k < 25 ? 0.0 : std::min(1.0, (k - 25) / 40.0);
    pf.on_pressure(t, altitude_to_pressure(alt0 + frac * rise_m));
  }
}

}  // namespace

TEST(ParticleFilter, PicksTheFloorFromRssi) {
  const Venue v = test::two_level_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi_on_level(pf, v, {15.0, 6.0}, 1, 60);
  const auto est = pf.estimate();
  EXPECT_EQ(est.level, 1);
  EXPECT_DOUBLE_EQ(est.z, 4.0);
  EXPECT_LT(distance(est.pos, {15.0, 6.0}), 1.5);
}

TEST(ParticleFilter, BarometerCannotChangeFloorOutsideConnectors) {
  const Venue v = test::two_level_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi_on_level(pf, v, {14.0, 5.0}, 0, 60);
  ASSERT_EQ(pf.estimate().level, 0);
  feed_rise(pf, 20.0, 4.0);
  EXPECT_EQ(pf.estimate().level, 0);
  double upstairs = 0.0;
  for (const auto& p : pf.particles()) {
    if (p.level != 0) upstairs += p.weight;
  }
  EXPECT_LT(upstairs, 0.05);
}

TEST(ParticleFilter, BarometerMovesFloorInsideStairwell) {
  const Venue v = test::two_level_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi_on_level(pf, v, {2.0, 1.5}, 0, 60);
  ASSERT_EQ(pf.estimate().level, 0);
  feed_rise(pf, 20.0, 4.0);
  EXPECT_EQ(pf.estimate().level, 1);
}

TEST(ParticleFilter, ConvergesOnStaticUser) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi(pf, v, {5.0, 5.0}, 60);
  const auto est = pf.estimate();
  EXPECT_TRUE(est.valid);
  EXPECT_LT(distance(est.pos, {5.0, 5.0}), 1.0);
  EXPECT_LT(est.sigma_radius(), 1.5);
}

TEST(ParticleFilter, WallConstraintRemovesParticlesThatCrossWalls) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi(pf, v, {8.0, 2.0}, 60);
  for (int i = 0; i < 10; ++i) pf.on_heading(0.1 * i, 0.0, 0.1);  // facing +x, towards the wall
  ASSERT_LT(pf.estimate().pos.x, 10.0);

  StepEvent step;
  step.t = 1.0;
  step.length = 2.5;
  pf.on_step(step);
  ASSERT_EQ(pf.recovery_count(), 0u);
  for (const auto& p : pf.particles()) {
    if (p.weight > 0.0 && p.pos.y < 4.5) {
      EXPECT_LT(p.pos.x, 10.0);
    }
  }
}

TEST(ParticleFilter, RecoversWhenEveryParticleIsInvalidated) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi(pf, v, {8.0, 2.0}, 60);
  // Longer than the venue diagonal: no hypothesis can survive the move.
  StepEvent step;
  step.t = 1.0;
  step.length = 30.0;
  pf.on_step(step);
  EXPECT_EQ(pf.recovery_count(), 1u);
  EXPECT_NEAR(pf.effective_sample_size(), static_cast<double>(pf.particles().size()), 1e-6);
}

TEST(ParticleFilter, DeterministicForSeed) {
  const Venue v = test::two_room_venue();
  ParticleFilter a(v, test_config(), {});
  ParticleFilter b(v, test_config(), {});
  auto cfg = test_config();
  cfg.seed = 12;
  ParticleFilter c(v, cfg, {});
  for (auto* pf : {&a, &b, &c}) feed_rssi(*pf, v, {15.0, 6.0}, 10);
  EXPECT_EQ(a.estimate().pos.x, b.estimate().pos.x);
  EXPECT_EQ(a.estimate().pos.y, b.estimate().pos.y);
  EXPECT_NE(a.estimate().pos.x, c.estimate().pos.x);
}

TEST(ParticleFilter, WeightsStayNormalised) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi(pf, v, {3.0, 7.0}, 20);
  double sum = 0.0;
  for (const auto& p : pf.particles()) sum += p.weight;
  EXPECT_NEAR(sum, 1.0, 1e-9);
  EXPECT_GT(pf.resample_count(), 0u);
}

TEST(ParticleFilter, IntegrityMonitorExcludesBiasedBeacon) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_rssi(pf, v, {5.0, 5.0}, 40);
  ASSERT_FALSE(pf.beacon_excluded(0));
  feed_rssi(pf, v, {5.0, 5.0}, 40, 10.0, /*bias_a=*/+18.0);
  EXPECT_TRUE(pf.beacon_excluded(0));
  for (std::size_t i = 1; i < v.beacons().size(); ++i) EXPECT_FALSE(pf.beacon_excluded(i));
  EXPECT_LT(distance(pf.estimate().pos, {5.0, 5.0}), 1.5);
}
