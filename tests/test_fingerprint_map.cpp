#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "ipe/fingerprint_map.hpp"
#include "ipe/particle_filter.hpp"
#include "ipe/venue.hpp"

using namespace ipe;

namespace {

// Four training scans along a 40 m corridor; "c" is heard everywhere but has no model.
Venue corridor() {
  return parse_venue(R"({
    "bounds": [0, 0, 40, 10],
    "beacons": [{"id": "a", "x": 0, "y": 5, "tx_power": -40}, {"id": "b", "x": 40, "y": 5, "tx_power": -40},
                {"id": "c", "x": 20, "y": 5, "model": false}],
    "floor_fingerprints": {"ids": ["a", "b", "c"],
      "scans": [[0, 0, -45, 1, -85, 2, -60], [0, 0, -60, 1, -75, 2, -52], [0, 0, -75, 1, -60, 2, -52],
                [0, 0, -85, 1, -45, 2, -60]],
      "xy": [[2, 5], [14, 5], [26, 5], [38, 5]]}
  })");
}

}  // namespace

TEST(FingerprintMap, NeedsPositions) {
  const Venue with = corridor();
  EXPECT_TRUE(FingerprintMap(with, {}).available());
  const Venue without = parse_venue(R"({"bounds": [0, 0, 10, 10], "beacons": [{"id": "a", "x": 1, "y": 1}],
                                        "floor_fingerprints": {"ids": ["a"], "scans": [[0, 0, -50]]}})");
  FingerprintMap map(without, {});
  EXPECT_FALSE(map.available());
  std::array<FingerprintNeighbour, 4> out{};
  EXPECT_EQ(map.nearest(std::vector<RssiReading>{{0, -50.0}}, out), 0);
}

TEST(FingerprintMap, ReturnsTheNearestScansInOrderWithWeightsSummingToOne) {
  const Venue v = corridor();
  FingerprintMap map(v, {3, 20, -100.0, 2});
  std::array<FingerprintNeighbour, 8> out{};
  const int n = map.nearest(std::vector<RssiReading>{{0, -62.0}, {1, -74.0}, {2, -53.0}}, out);
  ASSERT_EQ(n, 3);
  EXPECT_DOUBLE_EQ(out[0].pos.x, 14.0);
  // RMS over the three compared readings: sqrt((2^2 + 1^2 + 1^2) / 3).
  EXPECT_NEAR(out[0].distance_db, std::sqrt(6.0 / 3.0), 1e-9);
  EXPECT_LE(out[0].distance_db, out[1].distance_db);
  EXPECT_LE(out[1].distance_db, out[2].distance_db);
  EXPECT_NEAR(out[0].weight + out[1].weight + out[2].weight, 1.0, 1e-12);
  EXPECT_GT(out[0].weight, out[1].weight);
}

TEST(FingerprintMap, MatchesBruteForceWithMissingReadings) {
  const Venue v = corridor();
  FingerprintMap map(v, {4, 20, -100.0, 1});
  // "b" is not heard: every fingerprint heard it, which the compared set (the scan's readings) ignores.
  const std::vector<RssiReading> scan{{0, -80.0}, {2, -58.0}};
  std::array<FingerprintNeighbour, 4> out{};
  ASSERT_EQ(map.nearest(scan, out), 4);
  const auto& fp = v.floor_fingerprints();
  for (int j = 0; j < 4; ++j) {
    // Find the fingerprint at this neighbour's position and recompute its distance.
    std::size_t i = 0;
    while (fp.pos[i].x != out[static_cast<std::size_t>(j)].pos.x) ++i;
    double d2 = 0.0;
    for (const auto& r : scan) {
      double heard = -100.0;
      for (auto k = fp.begin[i]; k < fp.begin[i + 1]; ++k) {
        if (fp.beacon[k] == r.beacon_index) heard = fp.rssi[k];
      }
      d2 += (r.rssi - heard) * (r.rssi - heard);
    }
    EXPECT_NEAR(out[static_cast<std::size_t>(j)].distance_db, std::sqrt(d2 / 2.0), 1e-9);
  }
  EXPECT_DOUBLE_EQ(out[0].pos.x, 38.0);
}

TEST(FingerprintMap, ComparesOnlyTheStrongestReadings) {
  const Venue v = corridor();
  // One compared reading: the strongest, "c" at -52, which the two middle scans match exactly.
  FingerprintMap map(v, {2, 1, -100.0, 1});
  std::array<FingerprintNeighbour, 2> out{};
  ASSERT_EQ(map.nearest(std::vector<RssiReading>{{0, -99.0}, {2, -52.0}, {1, -99.0}}, out), 2);
  EXPECT_NEAR(out[0].distance_db, 0.0, 1e-9);
  EXPECT_NEAR(out[1].distance_db, 0.0, 1e-9);
  EXPECT_TRUE(out[0].pos.x == 14.0 || out[0].pos.x == 26.0);
}

TEST(FingerprintMap, TooFewReadingsGiveNothingAndLeaveNoState) {
  const Venue v = corridor();
  FingerprintMap map(v, {});  // min_readings 3
  std::array<FingerprintNeighbour, 4> out{};
  EXPECT_EQ(map.nearest(std::vector<RssiReading>{{0, -50.0}, {7, -50.0}}, out), 0);
  // The rejected scan must not leak into the next one.
  ASSERT_EQ(map.nearest(std::vector<RssiReading>{{0, -85.0}, {1, -45.0}, {2, -60.0}}, out), 4);
  EXPECT_DOUBLE_EQ(out[0].pos.x, 38.0);
  EXPECT_NEAR(out[0].distance_db, 0.0, 1e-9);
}

TEST(FingerprintLikelihood, PullsTheCloudToTheMatchingTrainingScans) {
  const Venue v = corridor();
  ParticleFilterConfig cfg;
  cfg.num_particles = 2000;
  cfg.fingerprint_weight = 1.0;
  cfg.fingerprint_sigma_m = 3.0;
  cfg.fingerprint_k = 2;
  cfg.fingerprint_strongest = 3;
  cfg.fingerprint_model_weight = 0.0;  // the fingerprints alone
  ParticleFilter pf(v, cfg, {});
  const std::vector<RssiReading> scan{{0, -84.0}, {1, -46.0}, {2, -60.0}};
  for (int i = 0; i < 5; ++i) pf.on_rssi_scan(1.0 + i, scan);
  ASSERT_TRUE(pf.estimate().valid);
  EXPECT_GT(pf.estimate().pos.x, 30.0);
}

TEST(FingerprintLikelihood, AScanOfUnmodelledBeaconsStillUpdates) {
  const Venue v = corridor();
  ParticleFilterConfig cfg;
  cfg.num_particles = 500;
  ParticleFilter off(v, cfg, {});
  // Without the fingerprint likelihood a beacon without a model says nothing.
  off.on_rssi_scan(1.0, std::vector<RssiReading>{{2, -52.0}});
  off.on_rssi(2.0, 2, -52.0);
  EXPECT_FALSE(off.estimate().valid);

  // With it, the scan counts even when the model is switched off: "c" is one of its dimensions.
  cfg.fingerprint_weight = 1.0;
  cfg.fingerprint_model_weight = 0.0;
  ParticleFilter on(v, cfg, {});
  on.on_rssi_scan(1.0, std::vector<RssiReading>{{2, -52.0}, {0, -61.0}, {1, -74.0}});
  ASSERT_TRUE(on.estimate().valid);
  EXPECT_GT(on.estimate().pos.x, 8.0);
  EXPECT_LT(on.estimate().pos.x, 32.0);
}

TEST(FingerprintLikelihood, OffLeavesTheFilterBitIdentical) {
  // A venue with positions and a venue without them give the same filter when the weight is 0.
  const Venue with = corridor();
  const Venue without = parse_venue(R"({
    "bounds": [0, 0, 40, 10],
    "beacons": [{"id": "a", "x": 0, "y": 5, "tx_power": -40}, {"id": "b", "x": 40, "y": 5, "tx_power": -40},
                {"id": "c", "x": 20, "y": 5, "model": false}]
  })");
  ParticleFilterConfig cfg;
  cfg.num_particles = 300;
  ParticleFilter a(with, cfg, {});
  ParticleFilter b(without, cfg, {});
  const std::vector<RssiReading> scan{{0, -70.0}, {1, -66.0}, {2, -55.0}};
  for (int i = 0; i < 4; ++i) {
    a.on_rssi_scan(1.0 + i, scan);
    b.on_rssi_scan(1.0 + i, scan);
  }
  EXPECT_EQ(a.estimate().pos.x, b.estimate().pos.x);
  EXPECT_EQ(a.estimate().pos.y, b.estimate().pos.y);
}
