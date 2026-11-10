#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <numbers>

#include "ipe/grid_engine.hpp"
#include "ipe/grid_filter.hpp"
#include "ipe/rssi_model.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {

/// 40 x 24 m with only the outer walls, and four beacons.
Venue open_venue() {
  return parse_venue(R"({"name": "open", "bounds": [0, 0, 40, 24],
    "walls": [[0,0,40,0],[40,0,40,24],[40,24,0,24],[0,24,0,0]],
    "beacons": [{"id": "A", "x": 5, "y": 5}, {"id": "B", "x": 35, "y": 5},
                {"id": "C", "x": 35, "y": 19}, {"id": "D", "x": 5, "y": 19}]})");
}

/// 20 x 10 m split in two by a wall without a door at x = 10.
Venue split_venue() {
  return parse_venue(R"({"name": "split", "bounds": [0, 0, 20, 10],
    "walls": [[0,0,20,0],[20,0,20,10],[20,10,0,10],[0,10,0,0],[10,0,10,10]],
    "beacons": [{"id": "A", "x": 5, "y": 5}, {"id": "B", "x": 15, "y": 5}]})");
}

/// No noise, no recovery: moves are exact.
GridFilterConfig quiet_config() {
  GridFilterConfig cfg;
  cfg.heading_sigma_deg = 0.0;
  cfg.step_length_sigma = 0.0;
  cfg.step_length_rel_sigma = 0.0;
  cfg.teleport_rate_per_s = 0.0;
  return cfg;
}

StepEvent step(double t, double length, double turn = 0.0) {
  StepEvent s;
  s.t = t;
  s.length = length;
  s.heading_delta = turn;
  return s;
}

double total_mass(const GridFilter& f) {
  double sum = 0.0;
  for (double m : f.mass()) sum += m;
  return sum;
}

Vec2 mean_position(const GridFilter& f) {
  Vec2 sum{};
  for (std::size_t c = 0; c < f.map().size(); ++c) sum += f.centroid(c) * f.mass()[c];
  return sum;
}

double level_mass(const GridFilter& f, int level) {
  double sum = 0.0;
  for (std::size_t c = 0; c < f.map().size(); ++c) {
    if (f.map().cell(c).level == level) sum += f.mass()[c];
  }
  return sum;
}

}  // namespace

TEST(GridFilter, UniformPriorLeavesOutClosedRooms) {
  const Venue v = test::corridor_venue();
  GridFilterConfig cfg;
  auto map = std::make_shared<const GridMap>(v, cfg);
  const GridFilter f(map, cfg, {});
  EXPECT_NEAR(total_mass(f), 1.0, 1e-12);
  const double u = 1.0 / static_cast<double>(map->size());
  for (std::size_t c = 0; c < map->size(); ++c) {
    EXPECT_DOUBLE_EQ(f.mass()[c], u);
    EXPECT_FALSE(test::in_corridor_closed_room(map->centre(c))) << map->centre(c).x << "," << map->centre(c).y;
  }
  EXPECT_FALSE(f.estimate().valid);
}

TEST(GridFilter, StepsMoveTheMassExactly) {
  const Venue v = open_venue();
  GridFilterConfig cfg = quiet_config();
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter f(map, cfg, {});
  f.set_point({5.0, 12.0}, 0, 0);
  for (int i = 0; i < 10; ++i) f.on_step(step(0.5 * (i + 1), 0.7));
  EXPECT_NEAR(total_mass(f), 1.0, 1e-12);
  EXPECT_NEAR(mean_position(f).x, 12.0, 1e-9);
  EXPECT_NEAR(mean_position(f).y, 12.0, 1e-9);
  EXPECT_NEAR(f.step_motion().x, 0.7, 1e-9);

  // Along-track noise spreads the mass but leaves its mean where it was.
  cfg.step_length_sigma = 0.08;
  cfg.step_length_rel_sigma = 0.1;
  GridFilter g(map, cfg, {});
  g.set_point({5.0, 12.0}, 0, 0);
  for (int i = 0; i < 10; ++i) g.on_step(step(0.5 * (i + 1), 0.7));
  EXPECT_NEAR(mean_position(g).x, 12.0, 1e-9);
  EXPECT_NEAR(mean_position(g).y, 12.0, 1e-9);
}

TEST(GridFilter, GyroTurnsShiftTheHeadingWithoutDiffusion) {
  const Venue v = open_venue();
  GridFilterConfig cfg = quiet_config();
  cfg.heading_sigma_deg = 3.0;
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter f(map, cfg, {});
  f.set_point({20.25, 12.25}, 0, 0);  // a cell centre: no move, so the mass stays in that cell
  const int n = 20;
  for (int i = 0; i < n; ++i) f.on_step(step(0.5 * (i + 1), 0.0, i == 0 ? 0.5 : 0.01));
  EXPECT_NEAR(f.bin_heading(0), 0.5 + 0.01 * (n - 1), 1e-12);

  const std::int32_t c = map->locate({20.25, 12.25}, 0);
  ASSERT_GE(c, 0);
  const int bins = cfg.heading_bins;
  double sum = 0.0, var = 0.0;
  for (int k = 0; k < bins; ++k) {
    const int signed_k = k <= bins / 2 ? k : k - bins;
    const double p = f.heading_prob(static_cast<std::size_t>(c), k);
    const double deg = 360.0 * signed_k / bins;
    sum += p;
    var += p * deg * deg;
  }
  EXPECT_NEAR(sum, 1.0, 1e-5);
  EXPECT_NEAR(var, n * 3.0 * 3.0, 0.01 * n * 9.0);
}

TEST(GridFilter, NoMassCrossesAWall) {
  const Venue v = split_venue();
  GridFilterConfig cfg;
  cfg.teleport_rate_per_s = 0.0;
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter f(map, cfg, {});
  // Laps of a 2.8 m square in the west half, which the walls keep the mass in.
  f.set_point({4.0, 4.0}, 0, 0);
  for (int i = 0; i < 40; ++i) {
    f.on_step(step(0.5 * (i + 1), 0.7, i % 4 == 0 && i > 0 ? std::numbers::pi / 2 : 0.0));
    ASSERT_NEAR(total_mass(f), 1.0, 1e-9);
    double east = 0.0;
    for (std::size_t c = 0; c < map->size(); ++c) {
      if (f.centroid(c).x > 10.0) east += f.mass()[c];
    }
    ASSERT_EQ(east, 0.0) << "step " << i;
  }
}

TEST(GridFilter, LevelChangesOnlyInsideConnectors) {
  const Venue v = test::two_level_venue();
  GridFilterConfig cfg = quiet_config();
  auto map = std::make_shared<const GridMap>(v, cfg);
  const int ground = *v.level_index(0), first = *v.level_index(1);

  GridFilter outside(map, cfg, {});
  outside.set_point({15.0, 5.0}, ground, 0);
  for (int i = 0; i < 5; ++i) outside.on_step(step(0.5 * (i + 1), 0.0));
  EXPECT_EQ(level_mass(outside, first), 0.0);

  GridFilter stairs(map, cfg, {});
  stairs.set_point({2.0, 1.5}, ground, 0);
  stairs.on_step(step(0.5, 0.0));
  EXPECT_NEAR(level_mass(stairs, first), cfg.level_change_prob_step, 1e-12);
}

TEST(GridFilter, FloorPriorPullsTheMassToTheClassifiedLevel) {
  // As the particle filter's test: the fingerprints say a beacon-A0 reading of -70 dBm is heard on level 1,
  // while the radio model (15 dB per slab) favours level 0, A0's own floor. The prior multiplies the odds of
  // level 1 by exactly p(1) / p(0) = 0.95 / 0.05 = 19 per scan, whatever the radio model does.
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
    GridFilterConfig cfg = quiet_config();
    cfg.floor_prior_weight = on ? 1.0 : 0.0;
    auto map = std::make_shared<const GridMap>(v, cfg);
    GridFilter f(map, cfg, {});
    for (int k = 0; k < 5; ++k) f.on_rssi_scan(2.0 * k, scan);
    share[on] = level_mass(f, 1);
  }
  EXPECT_LT(share[0], 0.1);
  const auto odds = [](double p) { return p / (1.0 - p); };
  EXPECT_NEAR(std::log(odds(share[1]) / odds(share[0])), 5.0 * std::log(19.0), 1e-6);
}

TEST(GridEngine, FloorPriorFollowsTheEngineConfigUnlessSet) {
  const Venue v = test::two_level_venue();
  EngineConfig base;
  base.pf.floor_prior_weight = 3.0;
  base.pf.floor_prior_k = 7;
  GridFilterConfig cfg;
  auto map = std::make_shared<const GridMap>(v, cfg);
  const GridEngine follows(v, map, base, cfg);
  EXPECT_EQ(follows.filter().config().floor_prior_weight, 3.0);
  EXPECT_EQ(follows.filter().config().floor_prior_k, 7);
  cfg.floor_prior_weight = 0.0;
  const GridEngine off(v, map, base, cfg);
  EXPECT_EQ(off.filter().config().floor_prior_weight, 0.0);
}

TEST(GridFilter, LearnsTheDeviceOffsetPerCell) {
  const Venue v = open_venue();
  GridFilterConfig cfg = quiet_config();
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter f(map, cfg, {});
  const Vec2 p{12.25, 9.75};
  f.set_point(p, 0, 0);
  const RssiModel model(v);
  double t = 0.0;
  for (int r = 0; r < 40; ++r) {
    for (std::size_t b = 0; b < v.beacons().size(); ++b) {
      f.on_rssi(t, b, model.expected_rssi(v.beacons()[b], p) + 9.0);
      t += 0.05;
    }
  }
  const auto c = static_cast<std::size_t>(map->locate(p, 0));
  EXPECT_NEAR(f.rssi_offset_mean(c), 9.0, 0.5);
  EXPECT_LT(f.rssi_offset_var(c), 1.0);
  EXPECT_TRUE(f.estimate().valid);
}

TEST(GridFilter, ConvergesOnAStaticUserFromTheUniformPrior) {
  const Venue v = test::two_room_venue();
  GridFilterConfig cfg;
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter f(map, cfg, {});
  const Vec2 p{14.0, 6.0};
  const RssiModel model(v);
  double t = 0.0;
  for (int r = 0; r < 30; ++r) {
    for (std::size_t b = 0; b < v.beacons().size(); ++b) {
      f.on_rssi(t, b, model.expected_rssi(v.beacons()[b], p));
      t += 0.05;
    }
  }
  EXPECT_LT(distance(f.estimate().pos, p), 1.0);
}

TEST(GridFilter, PredictsTheRadioModelAtTheCellCentre) {
  const Venue v = test::two_room_venue();
  GridFilterConfig cfg;
  const GridMap map(v, cfg);
  const RssiModel model(v);
  for (std::size_t b = 0; b < v.beacons().size(); ++b) {
    const auto walls = map.wall_counts(b);
    for (std::size_t c = 0; c < map.size(); ++c) {
      const Vec2 p = map.centre(c);
      const double free = model.expected_rssi_free_space(v.beacons()[b], distance(p, v.beacons()[b].pos));
      ASSERT_DOUBLE_EQ(free - v.wall_attenuation_db() * walls[c], model.expected_rssi(v.beacons()[b], p));
    }
  }
}

TEST(GridFilter, IsDeterministic) {
  const Venue v = test::two_room_venue();
  GridFilterConfig cfg;
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridFilter a(map, cfg, {}), b(map, cfg, {});
  const RssiModel model(v);
  for (GridFilter* f : {&a, &b}) {
    double t = 0.0;
    for (int i = 0; i < 20; ++i) {
      t += 0.5;
      f->on_step(step(t, 0.65, 0.05));
      f->on_heading(t, 0.3, deg2rad(12.0));
      const Vec2 p{3.0 + 0.6 * i, 3.0};
      for (std::size_t k = 0; k < v.beacons().size(); ++k) {
        f->on_rssi(t + 0.01 * static_cast<double>(k), k, model.expected_rssi(v.beacons()[k], p));
      }
    }
  }
  for (std::size_t c = 0; c < map->size(); ++c) ASSERT_EQ(a.mass()[c], b.mass()[c]);
  EXPECT_EQ(a.estimate().pos.x, b.estimate().pos.x);
  EXPECT_EQ(a.estimate().pos.y, b.estimate().pos.y);
}

TEST(GridEngine, RoomProbabilityIsTheMassInTheRoom) {
  const Venue v = test::two_room_venue_with_rooms();
  GridFilterConfig cfg;
  auto map = std::make_shared<const GridMap>(v, cfg);
  GridEngine engine(v, map, EngineConfig{}, cfg);
  const RssiModel model(v);
  const Vec2 p{4.0, 6.0};
  double t = 0.0;
  for (int r = 0; r < 20; ++r) {
    for (std::size_t b = 0; b < v.beacons().size(); ++b) {
      engine.on_ble(t, b, model.expected_rssi(v.beacons()[b], p));
      t += 0.3;  // more than update_period_s: the tracker updates at every packet
    }
  }
  double west = 0.0;
  for (std::size_t c = 0; c < map->size(); ++c) {
    for (const std::uint32_t room : map->rooms(c)) {
      if (room == 0) west += engine.filter().mass()[c];
    }
  }
  ASSERT_NE(engine.room_tracker(), nullptr);
  EXPECT_NEAR(engine.room_tracker()->probabilities()[0], west, 1e-12);
  EXPECT_GT(west, 0.9);
}

TEST(GridConfig, RejectsUnknownKeys) {
  GridFilterConfig cfg;
  grid::apply_overrides(cfg, R"({"heading_bins": 72})");
  EXPECT_EQ(cfg.heading_bins, 72);
  EXPECT_THROW(grid::apply_overrides(cfg, R"({"heading_binz": 72})"), std::invalid_argument);
}
