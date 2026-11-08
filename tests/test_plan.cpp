#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

#include "ipe/plan.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

/// 10 x 10 m open room with a beacon in every corner.
Venue corner_beacons() {
  return parse_venue(R"({
    "name": "corners",
    "bounds": [0, 0, 10, 10],
    "walls": [],
    "beacons": [
      {"id": "A", "x": 0,  "y": 0},
      {"id": "B", "x": 10, "y": 0},
      {"id": "C", "x": 10, "y": 10},
      {"id": "D", "x": 0,  "y": 10}
    ]
  })");
}

double crlb_at(const Venue& v, plan::Location at, const plan::AnalysisConfig& cfg) {
  const std::vector<plan::Location> pts = {at};
  return plan::score_points(v, pts, cfg).front().crlb_m;
}

}  // namespace

TEST(Plan, CrlbMatchesClosedFormAtSquareCentre) {
  // Four beacons at distance d along the diagonals: J = 2 k^2 / sigma^2 * I with
  // k = 10 n / (ln10 d), so the RMS bound is sigma / k. The gradients cancel, so
  // an unknown offset costs nothing at the centre.
  const Venue v = corner_beacons();
  plan::AnalysisConfig cfg;
  cfg.min_rssi_dbm = -120.0;
  const double d = std::hypot(5.0, 5.0);
  const double expected = cfg.sigma_db * std::numbers::ln10 * d / (10.0 * 2.0);
  EXPECT_NEAR(crlb_at(v, {{5, 5}, 0}, cfg), expected, 1e-9);
  cfg.offset_sigma_db = 0.0;
  EXPECT_NEAR(crlb_at(v, {{5, 5}, 0}, cfg), expected, 1e-9);
}

TEST(Plan, UnknownOffsetNeedsThreeBeacons) {
  const Venue full = corner_beacons();
  std::vector<Beacon> two(full.beacons().begin(), full.beacons().begin() + 2);
  const Venue v(full.name(), full.bounds(), {}, two, 0.0);
  plan::AnalysisConfig cfg;
  cfg.min_rssi_dbm = -120.0;
  cfg.offset_sigma_db = 1e6;  // practically no prior
  EXPECT_GT(crlb_at(v, {{5, 3}, 0}, cfg), 1e3);
  cfg.offset_sigma_db = 0.0;
  EXPECT_TRUE(std::isfinite(crlb_at(v, {{5, 3}, 0}, cfg)));
}

TEST(Plan, WalkWindowPoolsBeaconsHeardAlongTheWay) {
  // Two beacons cannot fix a position and an unknown offset at one point, but
  // walking past them changes the geometry enough to do it.
  const Venue full = corner_beacons();
  std::vector<Beacon> two(full.beacons().begin(), full.beacons().begin() + 2);
  const Venue v(full.name(), full.bounds(), {}, two, 0.0);
  plan::AnalysisConfig cfg;
  cfg.min_rssi_dbm = -120.0;
  cfg.offset_sigma_db = 1e6;
  cfg.window_m = 0.0;
  const std::vector<plan::Location> wps = {{{1, 5}, 0}, {{9, 5}, 0}};
  const auto path = plan::sample_path(wps, 1.0);

  const auto alone = plan::score_path(v, path, cfg);
  const auto points = plan::score_points(v, path, cfg);
  ASSERT_EQ(alone.size(), points.size());
  for (std::size_t i = 0; i < alone.size(); ++i) EXPECT_EQ(alone[i].crlb_m, points[i].crlb_m);
  EXPECT_GT(alone.back().crlb_m, 1e3);

  cfg.window_m = 4.0;
  const auto pooled = plan::score_path(v, path, cfg);
  EXPECT_GT(pooled.front().crlb_m, 1e3);  // nothing behind the first point yet
  EXPECT_LT(pooled.back().crlb_m, 50.0);
  cfg.window_m = 8.0;
  const auto longer = plan::score_path(v, path, cfg);
  for (std::size_t i = 0; i < path.size(); ++i) EXPECT_LE(longer[i].crlb_m, pooled[i].crlb_m + 1e-9);
}

TEST(Plan, RemovingBeaconsNeverTightensTheBound) {
  const Venue full = load_venue_file(test::data_path("venues/office_floor.json"));
  std::vector<Beacon> fewer(full.beacons().begin() + 1, full.beacons().end());
  const Venue thinned(full.name(), full.bounds(), full.levels(), fewer, full.connectors(), full.wall_attenuation_db(),
                      full.floor_attenuation_db());
  const plan::AnalysisConfig cfg;
  const auto grid = plan::venue_grid(full, 2.0);
  const auto a = plan::score_points(full, grid, cfg);
  const auto b = plan::score_points(thinned, grid, cfg);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_LE(b[i].heard, a[i].heard);
    EXPECT_GE(b[i].crlb_m, a[i].crlb_m - 1e-9);
  }
}

TEST(Plan, WallsHideBeacons) {
  const Venue v = test::two_room_venue();  // wall at x = 10 with a door at y 4.5-5.5
  plan::AnalysisConfig cfg;
  const std::vector<plan::Location> pts = {{{2, 8}, 0}};
  // Open-space range of a -59 dBm, n = 2 beacon: 20 m at -85 dBm. Beacon D at
  // (18, 8) is 16 m away; one wall (4 dB) pushes it below -86 dBm.
  cfg.min_rssi_dbm = -86.0;
  cfg.wall_aware = false;
  EXPECT_EQ(plan::score_points(v, pts, cfg).front().heard, 4);
  cfg.wall_aware = true;
  EXPECT_EQ(plan::score_points(v, pts, cfg).front().heard, 3);
}

TEST(Plan, FloorMarginFavoursTheOwnLevel) {
  const Venue v = test::two_level_venue();
  plan::AnalysisConfig cfg;
  cfg.min_rssi_dbm = -100.0;  // hear the other level through the 15 dB slab
  const std::vector<plan::Location> pts = {{{2, 2}, 0}, {{2, 2}, 1}};
  const auto s = plan::score_points(v, pts, cfg);
  EXPECT_GT(s[0].floor_margin_db, 0.0);  // standing next to A0
  EXPECT_GT(s[1].floor_margin_db, 0.0);  // B1 at (7, 3) still beats A0 through the slab
  EXPECT_LT(s[1].floor_margin_db, s[0].floor_margin_db);
}

TEST(Plan, GridStaysInsideTheFootprint) {
  const Venue box = test::two_room_venue();
  EXPECT_EQ(plan::venue_grid(box, 1.0).size(), 200u);
  const Venue l = test::with_footprint(box, test::l_footprint());
  EXPECT_EQ(plan::venue_grid(l, 1.0).size(), 150u);
  EXPECT_EQ(plan::venue_grid(test::two_level_venue(), 1.0).size(), 400u);
}

TEST(Plan, PathSamplingSkipsLevelChanges) {
  const std::vector<plan::Location> wps = {{{0, 0}, 0}, {{4, 0}, 0}, {{4, 0}, 1}, {{4, 3}, 1}};
  const auto pts = plan::sample_path(wps, 1.0);
  // 0..4 on level 0 (5 points), then (4,0) and 3 more on level 1.
  ASSERT_EQ(pts.size(), 9u);
  EXPECT_EQ(pts[4].level, 0);
  EXPECT_EQ(pts[5].level, 1);
  EXPECT_DOUBLE_EQ(pts.back().pos.y, 3.0);
}

TEST(Plan, SummaryCountsUnboundedPoints) {
  plan::AnalysisConfig cfg;
  cfg.target_m = 2.0;
  std::vector<plan::PointScore> s(4);
  s[0].crlb_m = 1.0;
  s[1].crlb_m = 3.0;
  s[2].crlb_m = std::numeric_limits<double>::infinity();
  s[3].crlb_m = 1.5;
  for (auto& p : s) p.heard = 3;
  s[2].heard = 1;
  const auto sum = plan::summarize(s, cfg);
  EXPECT_EQ(sum.points, 4u);
  EXPECT_DOUBLE_EQ(sum.share_within_target, 0.5);
  EXPECT_DOUBLE_EQ(sum.share_heard_below_3, 0.25);
  EXPECT_DOUBLE_EQ(sum.crlb_p50_m, 1.5);
  EXPECT_TRUE(std::isinf(sum.crlb_p95_m));
}
