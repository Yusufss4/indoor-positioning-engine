#include <gtest/gtest.h>

#include <cmath>

#include "ipe/ekf.hpp"
#include "ipe/rng.hpp"
#include "ipe/rssi_model.hpp"

using namespace ipe;

namespace {

Venue open_venue() {
  return parse_venue(R"({
    "name": "open", "bounds": [0, 0, 20, 20], "walls": [],
    "beacons": [{"id":"A","x":2,"y":2},{"id":"B","x":18,"y":2},{"id":"C","x":18,"y":18},{"id":"D","x":2,"y":18}]
  })");
}

void feed(Ekf& ekf, const Venue& v, Vec2 pos, int rounds, Rng* rng = nullptr, double t0 = 0.0) {
  RssiModelConfig cfg;
  cfg.wall_aware = false;
  const RssiModel model(v, cfg);
  double t = t0;
  for (int r = 0; r < rounds; ++r) {
    for (std::size_t i = 0; i < v.beacons().size(); ++i) {
      const double noise = rng ? rng->normal(0.0, 2.0) : 0.0;
      ekf.on_rssi(t, i, model.expected_rssi(v.beacons()[i], pos) + noise);
      t += 0.05;
    }
  }
}

void expect_valid_covariance(const Ekf::Mat3& P) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_GT(P[i * 3 + i], 0.0);
    for (int j = 0; j < 3; ++j) EXPECT_NEAR(P[i * 3 + j], P[j * 3 + i], 1e-12);
  }
  // Leading principal minors positive => positive definite.
  EXPECT_GT(P[0] * P[4] - P[1] * P[3], 0.0);
  const double det =
      P[0] * (P[4] * P[8] - P[5] * P[7]) - P[1] * (P[3] * P[8] - P[5] * P[6]) + P[2] * (P[3] * P[7] - P[4] * P[6]);
  EXPECT_GT(det, 0.0);
}

}  // namespace

TEST(Ekf, NotValidUntilEnoughBeaconsHeard) {
  const Venue v = open_venue();
  Ekf ekf(v, {}, {});
  EXPECT_FALSE(ekf.estimate().valid);
  ekf.on_rssi(0.0, 0, -65.0);
  ekf.on_rssi(0.1, 1, -75.0);
  EXPECT_FALSE(ekf.estimate().valid);
  ekf.on_rssi(0.2, 2, -75.0);
  EXPECT_TRUE(ekf.estimate().valid);
}

TEST(Ekf, ConvergesOnStaticUser) {
  const Venue v = open_venue();
  Ekf ekf(v, {}, {});
  Rng rng(4);
  feed(ekf, v, {7.0, 9.0}, 150, &rng);
  EXPECT_LT(distance(ekf.estimate().pos, {7.0, 9.0}), 1.0);
  expect_valid_covariance(ekf.covariance());
}

TEST(Ekf, CovarianceStaysPositiveDefiniteUnderMixedUpdates) {
  const Venue v = open_venue();
  Ekf ekf(v, {}, {});
  Rng rng(8);
  feed(ekf, v, {10.0, 10.0}, 5, &rng);
  double t = 1.0;
  for (int k = 0; k < 300; ++k) {
    StepEvent s;
    s.t = t += 0.5;
    s.length = 0.7;
    s.heading_delta = rng.normal(0.0, 0.2);
    ekf.on_step(s);
    ekf.on_heading(t, rng.uniform(-3.0, 3.0), 0.2);
    feed(ekf, v, ekf.estimate().pos, 1, &rng, t);
    expect_valid_covariance(ekf.covariance());
    if (HasFailure()) break;
  }
}

TEST(Ekf, StepGrowsUncertaintyAndHeadingShrinksIt) {
  const Venue v = open_venue();
  Ekf ekf(v, {}, {});
  feed(ekf, v, {10.0, 10.0}, 20);
  const auto before = ekf.covariance();
  StepEvent s;
  s.t = 10.0;
  s.length = 0.7;
  ekf.on_step(s);
  EXPECT_GT(ekf.covariance()[0] + ekf.covariance()[4], before[0] + before[4]);
  const double heading_var = ekf.covariance()[8];
  ekf.on_heading(10.1, ekf.state()[2], 0.1);
  EXPECT_LT(ekf.covariance()[8], heading_var);
}

TEST(Ekf, GatesOutlierRssi) {
  const Venue v = open_venue();
  Ekf ekf(v, {}, {});
  feed(ekf, v, {10.0, 10.0}, 60);
  const auto pos = ekf.estimate().pos;
  ekf.on_rssi(100.0, 0, -45.0);  // ~35 dB too strong for 11 m
  EXPECT_EQ(ekf.rejected_updates(), 1u);
  EXPECT_EQ(ekf.estimate().pos.x, pos.x);
}
