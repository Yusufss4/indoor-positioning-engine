#include "ipe/regression.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ipe/simulator.hpp"

namespace ipe::eval {
namespace fs = std::filesystem;

std::vector<BenchmarkCase> run_benchmark(const std::string& scenario_dir, const BenchmarkOptions& opts) {
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(scenario_dir)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  if (files.empty()) throw std::runtime_error("no scenario files in " + scenario_dir);
  std::sort(files.begin(), files.end());

  std::vector<sim::Scenario> scenarios;
  std::map<std::string, std::shared_ptr<const Venue>> venues;
  for (const auto& f : files) {
    auto sc = sim::load_scenario_file(f.string());
    if (!venues.count(sc.venue_path)) venues[sc.venue_path] = std::make_shared<Venue>(load_venue_file(sc.venue_path));
    scenarios.push_back(std::move(sc));
  }

  std::vector<EngineConfig> configs;
  for (const auto& p : opts.presets) {
    configs.push_back(preset_config(p));
    if (!opts.config_patch.empty()) apply_config_overrides(configs.back(), opts.config_patch);
  }

  // One job per (scenario, seed): simulate once, replay through every preset.
  struct JobResult {
    std::size_t scenario;
    std::vector<Metrics> per_preset;
  };
  auto job = [&](std::size_t si, int k) {
    const auto& sc = scenarios[si];
    const Venue& venue = *venues.at(sc.venue_path);
    const Trace trace = sim::simulate(sc, venue, sc.seed + static_cast<std::uint64_t>(k));
    JobResult r{si, {}};
    for (const auto& cfg : configs) r.per_preset.push_back(replay(venue, trace, cfg, opts.replay).metrics);
    return r;
  };

  std::vector<JobResult> job_results;
  if (opts.parallel) {
    std::vector<std::future<JobResult>> futures;
    for (std::size_t si = 0; si < scenarios.size(); ++si)
      for (int k = 0; k < opts.seeds; ++k) futures.push_back(std::async(std::launch::async, job, si, k));
    for (auto& f : futures) job_results.push_back(f.get());
  } else {
    for (std::size_t si = 0; si < scenarios.size(); ++si)
      for (int k = 0; k < opts.seeds; ++k) job_results.push_back(job(si, k));
  }

  std::vector<BenchmarkCase> cases;
  for (std::size_t si = 0; si < scenarios.size(); ++si) {
    for (std::size_t pi = 0; pi < configs.size(); ++pi) {
      BenchmarkCase c;
      c.scenario = scenarios[si].name;
      c.preset = opts.presets[pi];
      int converged = 0;
      for (const auto& jr : job_results) {
        if (jr.scenario != si) continue;
        const Metrics& m = jr.per_preset[pi];
        ++c.runs;
        c.mean_m += m.mean_m;
        c.cep50_m += m.cep50_m;
        c.cep95_m += m.cep95_m;
        c.availability += m.availability;
        c.compute_us_per_update += m.compute_us_per_update;
        if (m.convergence_s >= 0.0) {
          c.convergence_s += m.convergence_s;
          ++converged;
        } else {
          ++c.unconverged_runs;
        }
      }
      const double n = std::max(1, c.runs);
      c.mean_m /= n;
      c.cep50_m /= n;
      c.cep95_m /= n;
      c.availability /= n;
      c.compute_us_per_update /= n;
      c.convergence_s = converged ? c.convergence_s / converged : -1.0;
      cases.push_back(c);
    }
  }
  return cases;
}

std::string to_string(Finding::Status s) {
  switch (s) {
    case Finding::Status::Ok:
      return "ok";
    case Finding::Status::Improved:
      return "improved";
    case Finding::Status::Regressed:
      return "REGRESSED";
    case Finding::Status::Missing:
      return "missing";
    case Finding::Status::New:
      return "new";
  }
  return "?";
}

std::vector<Finding> compare_to_baseline(const std::vector<BenchmarkCase>& results, const std::string& baseline_json,
                                         const Tolerance& tol) {
  const auto base = nlohmann::json::parse(baseline_json).at("results");
  std::vector<Finding> findings;
  for (const auto& c : results) {
    for (const auto& [metric, current] : {std::pair{"cep50_m", c.cep50_m}, std::pair{"cep95_m", c.cep95_m}}) {
      Finding f;
      f.scenario = c.scenario;
      f.preset = c.preset;
      f.metric = metric;
      f.current = current;
      if (!base.contains(c.scenario) || !base[c.scenario].contains(c.preset)) {
        f.status = Finding::Status::New;
      } else {
        f.baseline = base[c.scenario][c.preset].at(metric).get<double>();
        const double limit = f.baseline * (1.0 + tol.relative) + tol.absolute_m;
        const double improved = f.baseline * (1.0 - tol.relative) - tol.absolute_m;
        if (current > limit) {
          f.status = Finding::Status::Regressed;
        } else if (current < improved) {
          f.status = Finding::Status::Improved;
        }
      }
      findings.push_back(f);
    }
  }
  for (const auto& [scenario, presets] : base.items()) {
    for (const auto& [preset, _] : presets.items()) {
      const bool present = std::any_of(results.begin(), results.end(), [&](const BenchmarkCase& c) {
        return c.scenario == scenario && c.preset == preset;
      });
      if (!present) findings.push_back({Finding::Status::Missing, scenario, preset, "*", 0.0, 0.0});
    }
  }
  return findings;
}

std::string benchmark_to_json(const std::vector<BenchmarkCase>& results, const BenchmarkOptions& opts, int indent) {
  nlohmann::json j;
  j["seeds"] = opts.seeds;
  j["warmup_s"] = opts.replay.warmup_s;
  nlohmann::json& r = j["results"];
  r = nlohmann::json::object();
  auto round3 = [](double v) { return std::round(v * 1000.0) / 1000.0; };
  for (const auto& c : results) {
    r[c.scenario][c.preset] = {{"mean_m", round3(c.mean_m)},
                               {"cep50_m", round3(c.cep50_m)},
                               {"cep95_m", round3(c.cep95_m)},
                               {"availability", round3(c.availability)},
                               {"convergence_s", round3(c.convergence_s)},
                               {"unconverged_runs", c.unconverged_runs}};
  }
  return j.dump(indent);
}

std::string benchmark_to_markdown(const std::vector<BenchmarkCase>& results) {
  std::ostringstream out;
  out << "| Scenario | Filter | Mean [m] | CEP50 [m] | CEP95 [m] | Converged in [s] | us/update |\n";
  out << "|---|---|---:|---:|---:|---:|---:|\n";
  char buf[256];
  for (const auto& c : results) {
    char conv[32];
    if (c.convergence_s < 0.0) {
      std::snprintf(conv, sizeof(conv), "never");
    } else if (c.unconverged_runs > 0) {
      std::snprintf(conv, sizeof(conv), "%.1f (%d/%d)", c.convergence_s, c.runs - c.unconverged_runs, c.runs);
    } else {
      std::snprintf(conv, sizeof(conv), "%.1f", c.convergence_s);
    }
    std::snprintf(buf, sizeof(buf), "| %s | %s | %.2f | %.2f | %.2f | %s | %.1f |\n", c.scenario.c_str(),
                  c.preset.c_str(), c.mean_m, c.cep50_m, c.cep95_m, conv, c.compute_us_per_update);
    out << buf;
  }
  return out.str();
}

}  // namespace ipe::eval
