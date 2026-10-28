#include <gtest/gtest.h>

#include <stdexcept>

#include "ipe/eval.hpp"
#include "ipe/regression.hpp"
#include "ipe/simulator.hpp"
#include "test_util.hpp"

using namespace ipe;
using namespace ipe::eval;

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
