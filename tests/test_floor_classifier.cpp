#include <gtest/gtest.h>

#include <vector>

#include "ipe/floor_classifier.hpp"
#include "ipe/venue.hpp"

using namespace ipe;

namespace {

Venue fingerprinted_venue() {
  return parse_venue(R"({
    "bounds": [0, 0, 10, 10],
    "levels": [{"ordinal": 0, "name": "G", "walls": []}, {"ordinal": 1, "name": "F1", "walls": []},
               {"ordinal": 2, "name": "F2", "walls": []}],
    "beacons": [{"id": "a", "x": 1, "y": 1, "level": 0}, {"id": "b", "x": 9, "y": 9, "level": 1},
                {"id": "c", "x": 5, "y": 5, "level": 2}],
    "floor_fingerprints": {"ids": ["a", "b", "c"], "scans": [
      [0, 0, -45, 1, -80], [0, 0, -50, 1, -85], [0, 0, -48],
      [1, 0, -80, 1, -45], [1, 1, -50], [1, 0, -85, 1, -48],
      [2, 2, -40]]}
  })");
}

}  // namespace

TEST(FloorClassifier, VotesForTheNearestFingerprints) {
  const Venue v = fingerprinted_venue();
  FloorClassifier c(v, {3, 0.0});
  ASSERT_TRUE(c.available());
  std::vector<double> p(3, -1.0);
  ASSERT_TRUE(c.classify(std::vector<RssiReading>{{0, -47.0}, {1, -82.0}}, p));
  EXPECT_DOUBLE_EQ(p[0], 1.0);
  EXPECT_DOUBLE_EQ(p[1], 0.0);
  ASSERT_TRUE(c.classify(std::vector<RssiReading>{{1, -47.0}}, p));
  EXPECT_GT(p[1], p[0]);
  EXPECT_NEAR(p[0] + p[1] + p[2], 1.0, 1e-12);
}

TEST(FloorClassifier, EpsSpreadsMassOverEveryLevel) {
  const Venue v = fingerprinted_venue();
  FloorClassifier c(v, {1, 0.3});
  std::vector<double> p(3);
  ASSERT_TRUE(c.classify(std::vector<RssiReading>{{2, -41.0}}, p));
  EXPECT_NEAR(p[2], 0.7 + 0.1, 1e-12);
  EXPECT_NEAR(p[0], 0.1, 1e-12);
  EXPECT_NEAR(p[1], 0.1, 1e-12);
}

TEST(FloorClassifier, MatchesBruteForceDistance) {
  // The sparse |s|^2 + |f|^2 - 2 s.f form ranks like the dense Euclidean distance with the fill.
  const Venue v = fingerprinted_venue();
  const auto& fp = v.floor_fingerprints();
  const std::vector<RssiReading> scan = {{0, -60.0}, {1, -62.0}, {2, -90.0}};
  double best = 1e300;
  int best_level = -1;
  for (std::size_t i = 0; i < fp.size(); ++i) {
    std::vector<double> f(3, -100.0);
    for (auto r = fp.begin[i]; r < fp.begin[i + 1]; ++r) f[fp.beacon[r]] = fp.rssi[r];
    double d = 0.0;
    for (const auto& r : scan) d += (r.rssi - f[r.beacon_index]) * (r.rssi - f[r.beacon_index]);
    if (d < best) {
      best = d;
      best_level = fp.level[i];
    }
  }
  FloorClassifier c(v, {1, 0.0});
  std::vector<double> p(3);
  ASSERT_TRUE(c.classify(scan, p));
  EXPECT_DOUBLE_EQ(p[static_cast<std::size_t>(best_level)], 1.0);
}

TEST(FloorClassifier, NothingToSayWithoutFingerprintedReadings) {
  const Venue v = fingerprinted_venue();
  FloorClassifier c(v, {});
  std::vector<double> p(3, 0.5);
  EXPECT_FALSE(c.classify(std::vector<RssiReading>{{7, -50.0}}, p));
  EXPECT_FALSE(c.classify({}, p));
  EXPECT_DOUBLE_EQ(p[0], 0.5);
  const Venue bare = parse_venue(R"({"bounds": [0, 0, 1, 1], "beacons": []})");
  EXPECT_FALSE(FloorClassifier(bare, {}).available());
}
