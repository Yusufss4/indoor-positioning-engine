#include <gtest/gtest.h>

#include <vector>

#include "ipe/map_estimate.hpp"

using namespace ipe;

namespace {

/// 20 x 10 m split by a solid wall at x = 10.
Venue split_venue() {
  return parse_venue(R"({"name": "split", "bounds": [0, 0, 20, 10],
    "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,10]], "beacons": []})");
}

std::vector<Particle> cluster(Vec2 c, int n, double w, int level = 0) {
  std::vector<Particle> out;
  for (int i = 0; i < n; ++i) {
    Particle p;
    p.pos = c + Vec2{0.1 * (i % 5), 0.1 * (i / 5)};
    p.weight = w;
    p.level = level;
    out.push_back(p);
  }
  return out;
}

Vec2 weighted_mean(const std::vector<Particle>& ps) {
  Vec2 m{};
  double w = 0.0;
  for (const auto& p : ps) {
    m += p.pos * p.weight;
    w += p.weight;
  }
  return m * (1.0 / w);
}

}  // namespace

TEST(MapEstimate, KeepsAMeanThatIsOnTheMap) {
  const Venue v = split_venue();
  MapConsistentEstimate est(v, 100);
  const auto ps = cluster({4, 4}, 20, 0.05);
  const Vec2 m = weighted_mean(ps);
  const Vec2 pos = est.position(ps, m, 0, false);
  EXPECT_DOUBLE_EQ(pos.x, m.x);
  EXPECT_DOUBLE_EQ(pos.y, m.y);
}

TEST(MapEstimate, MovesASplitMeanToTheHeavierGroup) {
  const Venue v = split_venue();
  MapConsistentEstimate est(v, 100);
  auto ps = cluster({7, 5}, 20, 0.03);           // west, 60% of the weight
  const auto east = cluster({15, 5}, 20, 0.02);  // east, 40%
  ps.insert(ps.end(), east.begin(), east.end());
  const Vec2 m = weighted_mean(ps);
  // The plain mean is just east of the wall, but its nearest particle is west of it.
  ASSERT_GT(m.x, 10.0);
  ASSERT_LT(m.x, 11.0);
  for (int call = 0; call < 2; ++call) {  // the histogram is cleared between calls
    const Vec2 pos = est.position(ps, m, 0, false);
    EXPECT_LT(pos.x, 10.0);
    EXPECT_NEAR(pos.x, 7.2, 0.5);
    EXPECT_FALSE(v.crosses_wall(pos, {7.0, 5.0}));
  }
}

TEST(MapEstimate, CountsOnlyParticlesOnTheLevel) {
  const Venue v = split_venue();
  MapConsistentEstimate est(v, 100);
  auto ps = cluster({4, 4}, 20, 0.025, 1);
  const auto other = cluster({15, 4}, 20, 0.025, 0);
  ps.insert(ps.end(), other.begin(), other.end());
  // The mean of the level-1 particles is in the west room; level-0 particles must not pull it.
  const Vec2 m = weighted_mean(cluster({4, 4}, 20, 0.025, 1));
  const Vec2 pos = est.position(ps, m, 1, true);
  EXPECT_DOUBLE_EQ(pos.x, m.x);
}
