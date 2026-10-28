#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "ipe/simulator.hpp"
#include "test_util.hpp"

using namespace ipe;

namespace {
std::vector<std::string> scenario_files() {
  std::vector<std::string> files;
  for (const auto& e : std::filesystem::directory_iterator(test::data_path("scenarios"))) {
    if (e.path().extension() == ".json") files.push_back(e.path().string());
  }
  std::sort(files.begin(), files.end());
  return files;
}
}  // namespace

// A walk that clips a wall would make the wall-constrained particle filter
// look artificially bad, so every shipped scenario is validated here.
TEST(Simulator, ShippedScenariosNeverWalkThroughWalls) {
  const auto files = scenario_files();
  ASSERT_GE(files.size(), 5u);
  for (const auto& f : files) {
    const auto sc = sim::load_scenario_file(f);
    const auto venue = load_venue_file(sc.venue_path);
    sim::SimulationReport rep;
    const auto trace = sim::simulate(sc, venue, std::nullopt, &rep);
    EXPECT_EQ(rep.wall_violations, 0) << sc.name;
    EXPECT_GT(rep.distance_m, 50.0) << sc.name;
    EXPECT_FALSE(trace.ble.empty()) << sc.name;
    for (const auto& s : trace.truth) EXPECT_TRUE(venue.bounds().contains(s.pos)) << sc.name;
    for (const auto& b : trace.ble) EXPECT_TRUE(venue.beacon_index(b.beacon_id).has_value()) << sc.name;
    if (venue.multi_level()) {
      EXPECT_GE(rep.level_changes, 2) << sc.name;
      EXPECT_FALSE(trace.baro.empty()) << sc.name;
    } else {
      EXPECT_EQ(rep.level_changes, 0) << sc.name;
      EXPECT_TRUE(trace.baro.empty()) << sc.name;
    }
  }
}

TEST(Simulator, StairsChangeLevelInsideTheStairwell) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_stairs_up_down.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  bool seen_upper = false;
  for (std::size_t i = 1; i < trace.truth.size(); ++i) {
    const auto& a = trace.truth[i - 1];
    const auto& b = trace.truth[i];
    seen_upper |= b.level == 1;
    EXPECT_GE(b.z, -1e-9);
    EXPECT_LE(b.z, 4.0 + 1e-9);
    if (a.level != b.level) {
      const Connector* c = venue.connector_at(b.pos, b.level);
      ASSERT_NE(c, nullptr) << "level change outside a connector at t=" << b.t;
      EXPECT_EQ(c->kind, ConnectorKind::Stairs);
    }
  }
  EXPECT_TRUE(seen_upper);
  EXPECT_EQ(trace.truth.back().level, 0);
}

TEST(Simulator, LiftRideIsStationaryAndChangesPressure) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_elevator.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  // Find the first upward ride: z strictly between the floors.
  std::size_t first = 0, last = 0;
  for (std::size_t i = 0; i < trace.truth.size(); ++i) {
    if (trace.truth[i].z > 0.05 && trace.truth[i].z < 3.95) {
      if (!first) first = i;
      last = i;
    }
    if (first && trace.truth[i].z >= 4.0 - 1e-9) break;
  }
  ASSERT_GT(first, 0u);
  EXPECT_LT(distance(trace.truth[first].pos, trace.truth[last].pos), 1e-9);
  const double ride_s = trace.truth[last].t - trace.truth[first].t;
  EXPECT_GT(ride_s, 2.0);
  EXPECT_LT(ride_s, 6.0);

  // Pressure before and after the ride differs by ~4 m worth (~0.47 hPa).
  auto mean_hpa = [&](double t0, double t1) {
    double s = 0.0;
    int n = 0;
    for (const auto& p : trace.baro) {
      if (p.t >= t0 && p.t < t1) s += p.hpa, ++n;
    }
    return s / std::max(n, 1);
  };
  const double t_start = trace.truth[first].t, t_end = trace.truth[last].t;
  EXPECT_NEAR(mean_hpa(t_start - 3.0, t_start) - mean_hpa(t_end + 1.0, t_end + 4.0), 0.47, 0.12);
}

TEST(Simulator, BarometerDoesNotPerturbOtherStreams) {
  auto sc = sim::load_scenario_file(test::data_path("scenarios/office_stairs_up_down.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto with = sim::simulate(sc, venue, 3);
  sc.baro.enabled = false;
  const auto without = sim::simulate(sc, venue, 3);
  EXPECT_TRUE(without.baro.empty());
  ASSERT_EQ(with.ble.size(), without.ble.size());
  for (std::size_t i = 0; i < with.ble.size(); ++i) ASSERT_EQ(with.ble[i].rssi, without.ble[i].rssi);
}

TEST(Simulator, RejectsLevelChangeOutsideConnector) {
  auto sc = sim::load_scenario_file(test::data_path("scenarios/office_stairs_up_down.json"));
  const auto venue = load_venue_file(sc.venue_path);
  sc.walk.waypoints = {{{10, 11.5}, 0}, {{12, 11.5}, 1}};
  EXPECT_THROW(sim::simulate(sc, venue), std::runtime_error);
  sc.walk.waypoints = {{{10, 11.5}, 0}, {{12, 11.5}, 5}};
  EXPECT_THROW(sim::simulate(sc, venue), std::runtime_error);
}

TEST(Simulator, DeterministicPerSeed) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto a = sim::simulate(sc, venue, 5);
  const auto b = sim::simulate(sc, venue, 5);
  const auto c = sim::simulate(sc, venue, 6);
  ASSERT_EQ(a.ble.size(), b.ble.size());
  for (std::size_t i = 0; i < a.ble.size(); ++i) ASSERT_EQ(a.ble[i].rssi, b.ble[i].rssi);
  EXPECT_NE(a.ble.size(), c.ble.size());
}

TEST(Simulator, DeadBeaconsAreSilent) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_field_issue.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  for (const auto& b : trace.ble) {
    EXPECT_NE(b.beacon_id, "C2");
    EXPECT_NE(b.beacon_id, "N3");
  }
  EXPECT_EQ(sc.ble.moved_beacons.count("S3"), 1u);
}

TEST(Simulator, DeviceOffsetShiftsRssi) {
  auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  sc.ble.dropout = 0.0;
  sc.ble.sensitivity_dbm = -200.0;
  const auto base = sim::simulate(sc, venue, 1);
  sc.ble.device_offset_db = -9.0;
  const auto shifted = sim::simulate(sc, venue, 1);
  ASSERT_EQ(base.ble.size(), shifted.ble.size());
  double diff = 0.0;
  for (std::size_t i = 0; i < base.ble.size(); ++i) diff += shifted.ble[i].rssi - base.ble[i].rssi;
  EXPECT_NEAR(diff / static_cast<double>(base.ble.size()), -9.0, 0.1);
}

TEST(Simulator, ScenarioValidation) {
  EXPECT_THROW(sim::parse_scenario(R"({"walk": {"waypoints": [[0, 0]]}})"), std::runtime_error);
  EXPECT_ANY_THROW(sim::parse_scenario(R"({"walk": {}})"));
}
