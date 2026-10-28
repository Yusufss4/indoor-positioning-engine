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
  }
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
