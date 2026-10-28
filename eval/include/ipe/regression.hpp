#pragma once

#include <string>
#include <vector>

#include "ipe/eval.hpp"

namespace ipe::eval {

/// Metrics for one (scenario, preset) pair averaged over several seeds.
struct BenchmarkCase {
  std::string scenario;
  std::string preset;
  int runs = 0;
  double mean_m = 0.0;
  double cep50_m = 0.0;
  double cep95_m = 0.0;
  double availability = 0.0;
  double convergence_s = 0.0;  ///< Mean over runs that converged.
  int unconverged_runs = 0;
  double compute_us_per_update = 0.0;
  bool multi_level = false;  ///< Floor metrics are only reported for multi-level venues.
  double floor_accuracy = 1.0;
  double floor_switch_latency_s = -1.0;
};

struct BenchmarkOptions {
  std::vector<std::string> presets = preset_names();
  int seeds = 1;  ///< Runs per scenario use seeds scenario.seed + k, k < seeds.
  ReplayOptions replay;
  bool parallel = true;
  std::string config_patch;  ///< Optional JSON merge patch applied to every preset (A/B experiments).
};

/// Simulates every scenario file in `scenario_dir` and replays it through
/// each preset. Results are sorted by scenario then preset order.
std::vector<BenchmarkCase> run_benchmark(const std::string& scenario_dir, const BenchmarkOptions& opts);

struct Tolerance {
  double relative = 0.10;        ///< Allowed fractional increase.
  double absolute_m = 0.15;      ///< Plus this many metres, to absorb tiny drifts.
  double floor_accuracy = 0.03;  ///< Allowed absolute drop of floor accuracy.
};

struct Finding {
  enum class Status { Ok, Improved, Regressed, Missing, New } status = Status::Ok;
  std::string scenario;
  std::string preset;
  std::string metric;
  double baseline = 0.0;
  double current = 0.0;
};

std::string to_string(Finding::Status s);

/// Compares CEP50 and CEP95 (and floor accuracy where the baseline has it) against the stored baseline.
std::vector<Finding> compare_to_baseline(const std::vector<BenchmarkCase>& results, const std::string& baseline_json,
                                         const Tolerance& tol);

std::string benchmark_to_json(const std::vector<BenchmarkCase>& results, const BenchmarkOptions& opts, int indent = 2);
std::string benchmark_to_markdown(const std::vector<BenchmarkCase>& results);

}  // namespace ipe::eval
