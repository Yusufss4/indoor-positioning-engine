#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

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

namespace {

/// 20 x 10 m split by a solid wall at x = 10, with one beacon on the wall.
Venue split_venue() {
  return parse_venue(R"({
    "name": "split",
    "bounds": [0, 0, 20, 10],
    "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,10]],
    "beacons": [{"id": "M", "x": 10, "y": 5}]
  })");
}

/// A ring of particles around the beacon, half on each side of the wall: its mean is on the wall.
void feed_split_cloud(ParticleFilter& pf, const Venue& v) {
  const RssiModel model(v);
  const double rssi = model.expected_rssi(v.beacons()[0], {7.0, 5.0});
  for (int k = 0; k < 30; ++k) pf.on_rssi(0.1 * k, 0, rssi);
}

}  // namespace

TEST(ParticleFilter, SplitCloudMeanLiesOnTheWallWithoutTheOption) {
  const Venue v = split_venue();
  auto cfg = test_config();
  cfg.map_consistent_estimate = false;
  ParticleFilter pf(v, cfg, {});
  feed_split_cloud(pf, v);
  ASSERT_TRUE(pf.estimate().valid);
  EXPECT_LT(std::abs(pf.estimate().pos.x - 10.0), 1.0);
}

TEST(ParticleFilter, SplitCloudEstimateStaysOffTheWall) {
  const Venue v = split_venue();
  ParticleFilter pf(v, test_config(), {});
  feed_split_cloud(pf, v);
  const auto est = pf.estimate();
  ASSERT_TRUE(est.valid);
  EXPECT_GT(std::abs(est.pos.x - 10.0), 0.05);
  EXPECT_TRUE(v.inside(est.pos));
  // On the side of the wall that holds particles, in sight of them.
  const bool west = est.pos.x < 10.0;
  double same_side = 0.0;
  for (const auto& p : pf.particles()) {
    if ((p.pos.x < 10.0) == west && !v.crosses_wall(est.pos, p.pos)) same_side += p.weight;
  }
  EXPECT_GT(same_side, 0.3);
  // Taken about the reported point, the covariance still reaches the other half of the ring.
  EXPECT_GT(std::sqrt(est.cov_xx), 2.0);
}

TEST(ParticleFilter, MapConsistentEstimateIsTheMeanForOneGroup) {
  const Venue v = test::two_room_venue();
  auto on = test_config();
  auto off = test_config();
  off.map_consistent_estimate = false;
  ParticleFilter a(v, on, {});
  ParticleFilter b(v, off, {});
  feed_rssi(a, v, {5.0, 5.0}, 60);
  feed_rssi(b, v, {5.0, 5.0}, 60);
  EXPECT_DOUBLE_EQ(a.estimate().pos.x, b.estimate().pos.x);
  EXPECT_DOUBLE_EQ(a.estimate().pos.y, b.estimate().pos.y);
  EXPECT_DOUBLE_EQ(a.estimate().cov_xx, b.estimate().cov_xx);
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

TEST(ParticleFilter, RingDensityIntegratesToOne) {
  // Brute-force Cartesian grid, far enough out that the tail is negligible.
  const double mean = std::log10(5.0), sigma = 0.2, h = 0.05;
  double sum = 0.0;
  for (double x = -40.0 + h / 2; x < 40.0; x += h) {
    for (double y = -40.0 + h / 2; y < 40.0; y += h) sum += ParticleFilter::ring_density(std::hypot(x, y), mean, sigma);
  }
  EXPECT_NEAR(sum * h * h, 1.0, 2e-3);
}

namespace {
ParticleFilterConfig proposal_config() {
  auto cfg = test_config();
  cfg.num_particles = 40000;
  cfg.estimate_rssi_offset = true;
  cfg.init_proposal = true;
  return cfg;
}

/// Weighted share of the particles inside [x0, x1] x [y0, y1].
double weighted_share(const ParticleFilter& pf, double x0, double x1, double y0, double y1) {
  double w = 0.0;
  for (const auto& p : pf.particles()) {
    if (p.pos.x >= x0 && p.pos.x < x1 && p.pos.y >= y0 && p.pos.y < y1) w += p.weight;
  }
  return w;
}
}  // namespace

TEST(ParticleFilter, BeaconSeedConcentratesParticlesInsideTheVenue) {
  const Venue v = test::two_room_venue();
  ParticleFilter pf(v, proposal_config(), {});
  // Beacon A at (2, 2); -59 dBm at 1 m and exponent 2 put -65 dBm at about 2 m.
  pf.seed_from_beacon(0, -65.0);
  ASSERT_TRUE(pf.radio_seeded());
  std::size_t near = 0;
  for (const auto& p : pf.particles()) {
    ASSERT_TRUE(v.bounds().contains(p.pos));
    if (distance(p.pos, {2.0, 2.0}) < 6.0) ++near;
  }
  // 6 m around A is under a fifth of the venue; the ring puts most draws there.
  EXPECT_GT(static_cast<double>(near) / static_cast<double>(pf.particles().size()), 0.5);
}

TEST(ParticleFilter, BeaconSeedWeightsRecoverTheUniformPrior) {
  // Importance weights undo the proposal: weighted, the cloud must still be
  // the uniform prior over position and the offset prior.
  const Venue v = test::two_room_venue();
  const auto cfg = proposal_config();
  ParticleFilter pf(v, cfg, {});
  pf.seed_from_beacon(0, -65.0);
  for (double x0 : {0.0, 5.0, 10.0, 15.0}) {
    for (double y0 : {0.0, 5.0}) EXPECT_NEAR(weighted_share(pf, x0, x0 + 5.0, y0, y0 + 5.0), 0.125, 0.02);
  }
  double mean = 0.0, var = 0.0;
  for (const auto& p : pf.particles()) mean += p.weight * p.rssi_offset;
  for (const auto& p : pf.particles()) var += p.weight * (p.rssi_offset - mean) * (p.rssi_offset - mean);
  EXPECT_NEAR(mean, 0.0, 0.5);
  EXPECT_NEAR(std::sqrt(var), cfg.rssi_offset_sigma, 0.5);
}

TEST(ParticleFilter, FootprintBoundsEveryDraw) {
  // Uniform start, beacon-ring seed and recovery all stay inside the L.
  const Venue v = test::with_footprint(test::two_room_venue(), test::l_footprint());
  const auto all_inside = [&](const ParticleFilter& pf) {
    return std::all_of(pf.particles().begin(), pf.particles().end(),
                       [&](const Particle& p) { return v.inside(p.pos); });
  };
  ParticleFilter pf(v, proposal_config(), {});
  EXPECT_TRUE(all_inside(pf));
  // Beacon D at (18, 8) is outside the footprint; its ring mostly misses it.
  pf.seed_from_beacon(3, -65.0);
  EXPECT_TRUE(all_inside(pf));

  ParticleFilter rec(v, test_config(), {});
  feed_rssi(rec, v, {5.0, 2.0}, 60);
  StepEvent step;
  step.t = 1.0;
  step.length = 30.0;
  rec.on_step(step);
  EXPECT_EQ(rec.recovery_count(), 1u);
  EXPECT_TRUE(all_inside(rec));
}

TEST(ParticleFilter, BeaconSeedWeightsRecoverTheFootprintPrior) {
  const Venue v = test::with_footprint(test::two_room_venue(), test::l_footprint());
  ParticleFilter pf(v, proposal_config(), {});
  pf.seed_from_beacon(0, -65.0);
  // Six 5 x 5 m cells make up the L, each with a sixth of the prior.
  for (double x0 : {0.0, 5.0, 10.0, 15.0}) {
    for (double y0 : {0.0, 5.0}) {
      const double expected = x0 >= 10.0 && y0 >= 5.0 ? 0.0 : 1.0 / 6.0;
      EXPECT_NEAR(weighted_share(pf, x0, x0 + 5.0, y0, y0 + 5.0), expected, 0.02) << x0 << "," << y0;
    }
  }
}

TEST(ParticleFilter, BeaconSeedWeightsRecoverTheLevelPrior) {
  const Venue v = test::two_level_venue();
  ParticleFilter pf(v, proposal_config(), {});
  pf.seed_from_beacon(4, -62.0);  // A1, on level index 1
  double upper = 0.0;
  std::size_t upper_count = 0;
  for (const auto& p : pf.particles()) {
    if (p.level == 1) {
      upper += p.weight;
      ++upper_count;
    }
  }
  EXPECT_GT(static_cast<double>(upper_count) / static_cast<double>(pf.particles().size()), 0.7);
  EXPECT_NEAR(upper, 0.5, 0.03);
}

TEST(ParticleFilter, LevelFootprintBoundsEveryDraw) {
  // Level 1 of the two-level venue shrunk to a 4 x 10 m landing at the east end (the lift side).
  const Venue base = test::two_level_venue();
  std::vector<Level> levels = base.levels();
  levels[1].footprint = {{16, 0}, {20, 0}, {20, 10}, {16, 10}};
  const Venue v(base.name(), base.bounds(), levels, base.beacons(), base.connectors(), base.wall_attenuation_db(),
                base.floor_attenuation_db());
  const auto all_inside = [&](const ParticleFilter& pf) {
    return std::all_of(pf.particles().begin(), pf.particles().end(),
                       [&](const Particle& p) { return v.inside(p.pos, p.level); });
  };
  ParticleFilter pf(v, test_config(), {});
  EXPECT_TRUE(all_inside(pf));
  // Levels are drawn by area: the 40 m^2 landing gets a sixth of the particles, not half.
  const auto upper =
      std::count_if(pf.particles().begin(), pf.particles().end(), [](const Particle& p) { return p.level == 1; });
  EXPECT_NEAR(static_cast<double>(upper) / static_cast<double>(pf.particles().size()), 40.0 / 240.0, 0.04);

  // Ring seed from an upper-level beacon: particles off the landing carry no weight.
  ParticleFilter seeded(v, proposal_config(), {});
  seeded.seed_from_beacon(4, -62.0);
  for (const auto& p : seeded.particles()) {
    if (!v.inside(p.pos, p.level)) {
      EXPECT_EQ(p.weight, 0.0);
    }
  }
}

TEST(ParticleFilter, BeaconSeedWaitsForTheInitWindow) {
  const Venue v = test::two_room_venue();
  auto cfg = proposal_config();
  cfg.num_particles = 800;
  cfg.init_window_s = 1.0;
  ParticleFilter pf(v, cfg, {});
  feed_rssi(pf, v, {5.0, 5.0}, 4);  // 16 packets over 0.8 s
  EXPECT_FALSE(pf.radio_seeded());
  feed_rssi(pf, v, {5.0, 5.0}, 60, 1.0);
  EXPECT_TRUE(pf.radio_seeded());
  // A static user with an unknown device offset is poorly observable, so
  // compare with the uniform start on the same packets instead of a fixed bound.
  cfg.init_proposal = false;
  ParticleFilter uniform(v, cfg, {});
  feed_rssi(uniform, v, {5.0, 5.0}, 4);
  feed_rssi(uniform, v, {5.0, 5.0}, 60, 1.0);
  EXPECT_LT(distance(pf.estimate().pos, {5.0, 5.0}), std::max(1.5, distance(uniform.estimate().pos, {5.0, 5.0})));
}

TEST(ParticleFilter, EstimateIsInvalidUntilRadioIsApplied) {
  const Venue v = test::two_room_venue();
  auto cfg = proposal_config();
  cfg.num_particles = 800;
  cfg.init_window_s = 1.0;
  ParticleFilter pf(v, cfg, {});
  EXPECT_FALSE(pf.estimate().valid);
  pf.on_heading(0.0, 0.0, 0.1);
  EXPECT_FALSE(pf.estimate().valid);
  feed_rssi(pf, v, {5.0, 5.0}, 4, 0.1);  // 16 packets over 0.8 s, still inside the init window
  EXPECT_FALSE(pf.estimate().valid);
  feed_rssi(pf, v, {5.0, 5.0}, 2, 1.2);
  ASSERT_TRUE(pf.radio_seeded());
  EXPECT_TRUE(pf.estimate().valid);

  cfg.init_proposal = false;
  ParticleFilter plain(v, cfg, {});
  EXPECT_FALSE(plain.estimate().valid);
  plain.on_rssi(0.0, 0, -70.0);
  EXPECT_TRUE(plain.estimate().valid);
}

TEST(ParticleFilter, ProposalSettingsAreInertWhenDisabled) {
  const Venue v = test::two_room_venue();
  auto tuned = test_config();
  tuned.recovery = true;
  tuned.init_proposal_fraction = 0.3;
  tuned.init_window_s = 0.0;
  tuned.proposal_history = 3;
  tuned.proposal_max_age_s = 10.0;
  auto plain = test_config();
  plain.recovery = true;
  ParticleFilter a(v, plain, {});
  ParticleFilter b(v, tuned, {});
  for (auto* pf : {&a, &b}) {
    feed_rssi(*pf, v, {15.0, 6.0}, 20);
    feed_rssi(*pf, v, {15.0, 6.0}, 20, 1.0, /*bias_a=*/+25.0);
  }
  ASSERT_EQ(a.particles().size(), b.particles().size());
  for (std::size_t i = 0; i < a.particles().size(); ++i) {
    EXPECT_EQ(a.particles()[i].pos.x, b.particles()[i].pos.x);
    EXPECT_EQ(a.particles()[i].weight, b.particles()[i].weight);
  }
}

TEST(ParticleFilter, ScanOfOneReadingMatchesSinglePacket) {
  const Venue v = test::two_room_venue();
  auto cfg = test_config();
  cfg.recovery = true;
  cfg.estimate_rssi_offset = true;
  ParticleFilter a(v, cfg, {});
  ParticleFilter b(v, cfg, {});
  const RssiModel model(v);
  double t = 0.0;
  for (int r = 0; r < 30; ++r) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) {
      const double rssi = model.expected_rssi(v.beacons()[i], {15.0, 6.0}) + (r % 3 == 0 ? 6.0 : 0.0);
      a.on_rssi(t, i, rssi);
      const RssiReading reading{i, rssi};
      b.on_rssi_scan(t, std::span<const RssiReading>(&reading, 1));
      t += 0.05;
    }
  }
  ASSERT_EQ(a.particles().size(), b.particles().size());
  for (std::size_t i = 0; i < a.particles().size(); ++i) {
    EXPECT_EQ(a.particles()[i].pos.x, b.particles()[i].pos.x);
    EXPECT_EQ(a.particles()[i].rssi_offset, b.particles()[i].rssi_offset);
    EXPECT_EQ(a.particles()[i].weight, b.particles()[i].weight);
  }
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

namespace {

double weight_on_level(const ParticleFilter& pf, int level) {
  double w = 0.0;
  for (const auto& p : pf.particles()) {
    if (p.level == level) w += p.weight;
  }
  return w;
}

/// Holds the pressure, then rises `rise_m` and falls back after `hold_s`.
void feed_blip(ParticleFilter& pf, double t0, double rise_m, double hold_s) {
  const double alt0 = pressure_to_altitude(1000.0);
  for (int k = 0; k <= 100; ++k) {
    const double t = t0 + 0.2 * k;
    const bool up = t >= t0 + 5.0 && t < t0 + 5.0 + hold_s;
    pf.on_pressure(t, altitude_to_pressure(alt0 + (up ? rise_m : 0.0)));
  }
}

}  // namespace

TEST(ParticleFilter, BaroJumpTakesTheNearestStairsFromMetresAway) {
  // As BarometerCannotChangeFloorOutsideConnectors, but with the jump on: the lift is ~6 m away.
  const Venue v = test::two_level_venue();
  auto cfg = test_config();
  cfg.baro_jump = true;
  ParticleFilter pf(v, cfg, {});
  feed_rssi_on_level(pf, v, {14.0, 5.0}, 0, 60);
  ASSERT_EQ(pf.estimate().level, 0);
  feed_rise(pf, 20.0, 4.0);
  EXPECT_EQ(pf.estimate().level, 1);
  EXPECT_GT(weight_on_level(pf, 1), 0.9);
}

TEST(ParticleFilter, BaroJumpNeedsAConnectorWithinTheRadius) {
  const Venue v = test::two_level_venue();
  auto cfg = test_config();
  cfg.baro_jump = true;
  cfg.baro_jump_radius_m = 2.0;
  ParticleFilter pf(v, cfg, {});
  feed_rssi_on_level(pf, v, {14.0, 5.0}, 0, 60);
  feed_rise(pf, 20.0, 4.0);
  EXPECT_LT(weight_on_level(pf, 1), 0.05);
}

TEST(ParticleFilter, BaroJumpIgnoresAShortExcursion) {
  const Venue v = test::two_level_venue();
  auto cfg = test_config();
  cfg.baro_jump = true;
  ParticleFilter pf(v, cfg, {});
  feed_rssi_on_level(pf, v, {14.0, 5.0}, 0, 60);
  feed_blip(pf, 20.0, 4.0, 1.0);
  EXPECT_EQ(pf.estimate().level, 0);
  EXPECT_LT(weight_on_level(pf, 1), 0.05);
}

TEST(ParticleFilter, ReseedKeepsTheBarometerReference) {
  // Settled on level 0 with the barometer running, then re-seeded around a level 1 beacon: with the
  // reference kept, the next pressure reading rejects the level 1 particles; without, they fit it.
  const Venue v = test::two_level_venue();
  const std::size_t a1 = 4;
  ASSERT_EQ(v.beacons()[a1].level, 1);
  for (const double keep : {0.0, 1.0}) {
    auto cfg = test_config();
    cfg.baro_reseed_keep_reference = keep;
    ParticleFilter pf(v, cfg, {});
    pf.on_pressure(0.0, 1000.0);
    feed_rssi_on_level(pf, v, {14.0, 5.0}, 0, 60);
    pf.on_pressure(20.0, 1000.0);
    ASSERT_EQ(pf.estimate().level, 0);
    pf.seed_from_beacon(a1, -60.0);
    const double before = weight_on_level(pf, 1);
    ASSERT_GT(before, 0.3);
    pf.on_pressure(21.0, 1000.0);
    if (keep > 0.0) {
      EXPECT_LT(weight_on_level(pf, 1), 0.05);
    } else {
      EXPECT_NEAR(weight_on_level(pf, 1), before, 0.1);
    }
  }
}

TEST(ParticleFilter, FloorPriorPullsTheCloudToTheClassifiedLevel) {
  // The fingerprints say a beacon-A0 reading of -70 dBm is heard on level 1, while the radio model
  // (15 dB per slab) is sure of level 0, A0's own floor: the prior moves weight to level 1 by orders of
  // magnitude, one factor of p(1) / p(0) = 19 per scan.
  Venue v = test::two_level_venue();
  FloorFingerprints fp;
  fp.begin.push_back(0);
  for (int i = 0; i < 6; ++i) {
    fp.level.push_back(1);
    fp.beacon.push_back(0);
    fp.rssi.push_back(-70.0F);
    fp.begin.push_back(static_cast<std::uint32_t>(fp.beacon.size()));
  }
  v.set_floor_fingerprints(fp);

  const std::vector<RssiReading> scan = {{0, -70.0}};
  double share[2] = {0.0, 0.0};
  for (const int on : {0, 1}) {
    auto cfg = test_config();
    cfg.floor_prior_weight = on ? 1.0 : 0.0;
    ParticleFilter pf(v, cfg, {});
    for (int k = 0; k < 5; ++k) pf.on_rssi_scan(2.0 * k, scan);
    share[on] = weight_on_level(pf, 1);
  }
  EXPECT_LT(share[0], 0.01);
  EXPECT_GT(share[1], 1000.0 * share[0]);
}

TEST(ParticleFilter, InitProposalTurnsOnForSparseClouds) {
  // two_room_venue: 200 m^2. The two-level venue counts both floors: 400 m^2.
  const Venue v = test::two_room_venue();
  const Venue two = test::two_level_venue();
  auto cfg = test_config();
  cfg.init_proposal_auto_density = 0.6;
  cfg.num_particles = 100;  // 0.5 per m^2
  EXPECT_TRUE(ParticleFilter(v, cfg, {}).init_proposal_active());
  cfg.num_particles = 200;  // 1.0 per m^2 here, 0.5 per m^2 over both floors
  EXPECT_FALSE(ParticleFilter(v, cfg, {}).init_proposal_active());
  EXPECT_TRUE(ParticleFilter(two, cfg, {}).init_proposal_active());
  cfg.init_proposal_auto_density = 0.0;
  cfg.num_particles = 10;
  EXPECT_FALSE(ParticleFilter(v, cfg, {}).init_proposal_active());
  cfg.init_proposal = true;  // set explicitly, on whatever the density
  cfg.num_particles = 5000;
  EXPECT_TRUE(ParticleFilter(v, cfg, {}).init_proposal_active());
}

TEST(ParticleFilter, CalibratesTheSlabLossFromReadings) {
  // The map says 15 dB per slab; the building loses 5. A walker standing on level 0.
  const Venue v = test::two_level_venue();
  const RssiModel model(v);
  const Vec2 pos{14.0, 5.0};
  double error_without = 0.0;
  for (const bool calibrate : {false, true}) {
    auto cfg = test_config();
    cfg.calibrate_floor_loss = calibrate;
    // At 15 dB the upstairs beacons are predicted below -85 dBm here; open the gate to test the estimator.
    cfg.loss_calibration_min_rssi_dbm = -120.0;
    ParticleFilter pf(v, cfg, {});
    double t = 0.0;
    for (int r = 0; r < 300; ++r) {
      for (std::size_t i = 0; i < v.beacons().size(); ++i) {
        const Beacon& b = v.beacons()[i];
        pf.on_rssi(t, i, model.expected_rssi(b, pos, 0, v.wall_attenuation_db(), 5.0));
        t += 0.05;
      }
    }
    EXPECT_EQ(pf.estimate().level, 0);
    const double error = distance(pf.estimate().pos, pos);
    if (calibrate) {
      EXPECT_NEAR(pf.floor_loss_db(), 5.0, 1.5);
      EXPECT_LT(error, 1.0);
      EXPECT_LT(error, error_without);  // the upstairs beacons no longer pull the estimate away
    } else {
      EXPECT_EQ(pf.floor_loss_db(), v.floor_attenuation_db());
      error_without = error;
    }
  }
}
