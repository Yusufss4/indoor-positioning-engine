#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "ipe/eval.hpp"
#include "ipe/regression.hpp"
#include "ipe/simulator.hpp"
#include "test_util.hpp"

using namespace ipe;
using namespace ipe::eval;
namespace fs = std::filesystem;

TEST(Eval, PercentileInterpolates) {
  EXPECT_DOUBLE_EQ(percentile({}, 0.5), 0.0);
  EXPECT_DOUBLE_EQ(percentile({3.0}, 0.95), 3.0);
  EXPECT_DOUBLE_EQ(percentile({4.0, 1.0, 3.0, 2.0}, 0.5), 2.5);
  EXPECT_DOUBLE_EQ(percentile({0.0, 10.0}, 0.95), 9.5);
}

TEST(Eval, MetricsRespectWarmupAndAvailability) {
  std::vector<EstimateRecord> recs;
  for (int i = 0; i < 100; ++i) {
    EstimateRecord r;
    r.t = i * 0.5;
    r.has_fix = i >= 4;
    r.error_m = i < 20 ? 10.0 : 1.0;
    recs.push_back(r);
  }
  const auto m = compute_metrics(recs, 10.0);
  EXPECT_EQ(m.samples, 80u);
  EXPECT_DOUBLE_EQ(m.availability, 1.0);
  EXPECT_DOUBLE_EQ(m.cep50_m, 1.0);
  EXPECT_DOUBLE_EQ(m.time_to_first_fix_s, 2.0);
  EXPECT_DOUBLE_EQ(m.convergence_s, 10.0);
  EXPECT_DOUBLE_EQ(m.floor_accuracy, 1.0);
  EXPECT_DOUBLE_EQ(m.floor_switch_latency_s, -1.0);
}

TEST(Eval, FloorMetricsCountWrongLevelSeparately) {
  // Truth goes up at t = 10 s; the estimate follows 2 s later.
  std::vector<EstimateRecord> recs;
  for (int i = 0; i < 40; ++i) {
    EstimateRecord r;
    r.t = i * 0.5;
    r.has_fix = true;
    r.truth_level = r.t >= 10.0 ? 1 : 0;
    r.est_level = r.t >= 12.0 ? 1 : 0;
    r.error_m = r.truth_level == r.est_level ? 1.0 : 30.0;
    recs.push_back(r);
  }
  const auto m = compute_metrics(recs, 0.0);
  EXPECT_EQ(m.level_changes, 1);
  EXPECT_DOUBLE_EQ(m.floor_switch_latency_s, 2.0);
  EXPECT_DOUBLE_EQ(m.floor_accuracy, 36.0 / 40.0);
  EXPECT_DOUBLE_EQ(m.max_m, 1.0);  // wrong-floor fixes are not horizontal errors
}

TEST(Eval, CalibrationFromReportedCovariance) {
  // Unit covariance, so NEES is the squared error: 0, 1, 4, 9.
  std::vector<EstimateRecord> recs;
  for (int i = 0; i < 4; ++i) {
    EstimateRecord r;
    r.t = i;
    r.has_fix = true;
    r.estimate = {static_cast<double>(i), 0.0};
    r.error_m = i;
    r.cov_xx = r.cov_yy = 1.0;
    recs.push_back(r);
  }
  const auto m = compute_metrics(recs, 0.0);
  EXPECT_DOUBLE_EQ(m.coverage95, 0.75);
  EXPECT_DOUBLE_EQ(m.nees_median, 2.5);

  // A correlated covariance stretched along x: an error along x is cheap, along y expensive.
  EstimateRecord r;
  r.has_fix = true;
  r.cov_xx = 4.0;
  r.cov_xy = 1.0;
  r.cov_yy = 1.0;
  r.estimate = {2.0, 0.0};
  EXPECT_DOUBLE_EQ(compute_metrics({r}, 0.0).nees_median, 4.0 / 3.0);
  r.estimate = {0.0, 2.0};
  EXPECT_DOUBLE_EQ(compute_metrics({r}, 0.0).nees_median, 16.0 / 3.0);

  for (auto& rec : recs) rec.cov_xx = rec.cov_yy = 0.0;
  const auto none = compute_metrics(recs, 0.0);
  EXPECT_DOUBLE_EQ(none.coverage95, -1.0);
  EXPECT_DOUBLE_EQ(none.nees_median, -1.0);
}

TEST(Eval, ReplayGroupsScans) {
  // Filters without a scan model see a scan as consecutive packets, so marking
  // every reading as part of a scan changes nothing for the EKF.
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  Trace scanned = trace;
  std::uint32_t id = 0;
  for (std::size_t b = 0; b < scanned.ble.size(); ++b) {
    if (b == 0 || scanned.ble[b].t != scanned.ble[b - 1].t) ++id;
    scanned.ble[b].scan = id;
  }
  const auto a = replay(venue, trace, preset_config("ekf"));
  const auto b = replay(venue, scanned, preset_config("ekf"));
  ASSERT_EQ(a.records.size(), b.records.size());
  for (std::size_t i = 0; i < a.records.size(); ++i) {
    EXPECT_EQ(a.records[i].estimate.x, b.records[i].estimate.x);
    EXPECT_EQ(a.records[i].estimate.y, b.records[i].estimate.y);
  }
  EXPECT_GT(a.metrics.coverage95, 0.0);
  EXPECT_DOUBLE_EQ(replay(venue, trace, preset_config("wcl")).metrics.coverage95, -1.0);
}

TEST(Eval, PresetsDiffer) {
  EXPECT_TRUE(preset_config("pf").pf.wall_constraint);
  EXPECT_FALSE(preset_config("pf-nomap").pf.wall_constraint);
  EXPECT_FALSE(preset_config("pf-nomap").rssi.wall_aware);
  EXPECT_EQ(preset_config("ekf").filter, FilterKind::Ekf);
  EXPECT_THROW(preset_config("kalman"), std::invalid_argument);
}

TEST(Eval, ConfigOverridesMergeAndRejectTypos) {
  auto cfg = preset_config("pf");
  apply_config_overrides(cfg, R"({"pf": {"num_particles": 321}, "rssi": {"sigma_db": 7.5}})");
  EXPECT_EQ(cfg.pf.num_particles, 321);
  EXPECT_DOUBLE_EQ(cfg.rssi.sigma_db, 7.5);
  EXPECT_TRUE(cfg.pf.wall_constraint);  // untouched keys keep their values
  EXPECT_THROW(apply_config_overrides(cfg, R"({"pf": {"num_particle": 10}})"), std::invalid_argument);
  EXPECT_THROW(apply_config_overrides(cfg, R"({"filter": "nope"})"), std::invalid_argument);
}

TEST(Eval, ConfigJsonRoundTrip) {
  auto cfg = preset_config("ekf");
  cfg.ekf.rssi_min_dbm = -80.0;
  auto copy = preset_config("pf");
  apply_config_overrides(copy, config_to_json(cfg));
  EXPECT_EQ(copy.filter, FilterKind::Ekf);
  EXPECT_DOUBLE_EQ(copy.ekf.rssi_min_dbm, -80.0);
}

TEST(Eval, ReplayProducesOneRecordPerTruthSample) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const auto trace = sim::simulate(sc, venue);
  ReplayOptions opts;
  opts.particle_snapshot_every = 10;
  const auto r = replay(venue, trace, preset_config("pf"), opts);
  EXPECT_EQ(r.records.size(), trace.truth.size());
  EXPECT_FALSE(r.snapshots.empty());
  EXPECT_GT(r.metrics.steps, 100u);
  // Sanity bound, not a tuning target: the regression suite owns accuracy.
  EXPECT_LT(r.metrics.cep50_m, 3.0);
}

TEST(Eval, TraceManifestGroupsRecordedTraces) {
  const auto sc = sim::load_scenario_file(test::data_path("scenarios/office_nominal.json"));
  const auto venue = load_venue_file(sc.venue_path);
  const fs::path dir = fs::temp_directory_path() / "ipe_trace_manifest_test";
  fs::create_directories(dir);
  for (int k = 0; k < 3; ++k)
    write_trace_file((dir / ("t" + std::to_string(k) + ".csv")).string(), sim::simulate(sc, venue, 200 + k));
  std::ofstream(dir / "manifest.json") << R"({"traces": [
    {"name": "a0", "group": "a", "venue": ")"
                                       << sc.venue_path << R"(", "trace": "t0.csv"},
    {"name": "a1", "group": "a", "venue": ")"
                                       << sc.venue_path << R"(", "trace": "t1.csv"},
    {"name": "b0", "group": "b", "venue": ")"
                                       << sc.venue_path << R"(", "trace": "t2.csv"}]})";

  BenchmarkOptions opts;
  opts.presets = {"wcl"};
  const auto cases = run_trace_benchmark((dir / "manifest.json").string(), opts);
  ASSERT_EQ(cases.size(), 3u);
  EXPECT_EQ(cases[0].scenario, "a");
  EXPECT_EQ(cases[0].runs, 2);
  EXPECT_EQ(cases[1].scenario, "b");
  EXPECT_EQ(cases[1].runs, 1);
  EXPECT_EQ(cases[2].scenario, "all");
  EXPECT_EQ(cases[2].runs, 3);
  EXPECT_TRUE(cases[2].recorded);
  EXPECT_GT(cases[2].median_cep50_m, 0.0);
  // One run: the median and the mean are the same number.
  EXPECT_DOUBLE_EQ(cases[1].median_cep50_m, cases[1].cep50_m);
  fs::remove_all(dir);
}

TEST(Eval, BaselineComparisonFlagsRegressions) {
  BenchmarkCase c;
  c.scenario = "s";
  c.preset = "pf";
  c.cep50_m = 1.0;
  c.cep95_m = 3.0;
  const std::string baseline = R"({"results": {"s": {"pf": {"cep50_m": 1.0, "cep95_m": 2.0}}, "gone": {"pf": {}}}})";
  const auto findings = compare_to_baseline({c}, baseline, {});
  int regressed = 0, missing = 0;
  for (const auto& f : findings) {
    regressed += f.status == Finding::Status::Regressed ? 1 : 0;
    missing += f.status == Finding::Status::Missing ? 1 : 0;
  }
  EXPECT_EQ(regressed, 1);
  EXPECT_EQ(missing, 1);
}
