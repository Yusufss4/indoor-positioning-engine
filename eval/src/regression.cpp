#include "ipe/regression.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ipe/simulator.hpp"

namespace ipe::eval {
namespace fs = std::filesystem;

namespace {
std::vector<EngineConfig> preset_configs(const BenchmarkOptions& opts) {
  std::vector<EngineConfig> configs;
  for (const auto& p : opts.presets) {
    configs.push_back(preset_config(p));
    for (const auto& patch : opts.config_patches) apply_config_overrides(configs.back(), patch);
  }
  return configs;
}

double mean_or(const std::vector<double>& values, double fallback) {
  if (values.empty()) return fallback;
  double sum = 0.0;
  for (double v : values) sum += v;
  return sum / static_cast<double>(values.size());
}
}  // namespace

namespace {

/// Collects the smoothness figures of the runs of one case.
struct SmoothnessSamples {
  std::vector<double> jitter_p50, jitter_p95, jumps, flips, changes, path_ratio;

  void add(const Metrics& m) {
    if (m.jitter_p50_m >= 0.0) jitter_p50.push_back(m.jitter_p50_m);
    if (m.jitter_p95_m >= 0.0) jitter_p95.push_back(m.jitter_p95_m);
    if (m.jumps_per_min >= 0.0) jumps.push_back(m.jumps_per_min);
    flips.push_back(m.level_flips);
    changes.push_back(m.level_changes);
    if (m.path_ratio >= 0.0) path_ratio.push_back(m.path_ratio);
  }

  /// Means (seeds) or medians (walks).
  void fill(BenchmarkCase& c, bool median) const {
    const auto agg = [&](const std::vector<double>& v, double none) {
      if (v.empty()) return none;
      if (median) return percentile(v, 0.5);
      double sum = 0.0;
      for (double x : v) sum += x;
      return sum / static_cast<double>(v.size());
    };
    c.jitter_p50_m = agg(jitter_p50, -1.0);
    c.jitter_p95_m = agg(jitter_p95, -1.0);
    c.jumps_per_min = agg(jumps, -1.0);
    c.level_flips = agg(flips, 0.0);
    c.level_changes = agg(changes, 0.0);
    c.path_ratio = agg(path_ratio, -1.0);
  }
};

}  // namespace

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

  const std::vector<EngineConfig> configs = preset_configs(opts);

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
      c.multi_level = venues.at(scenarios[si].venue_path)->multi_level();
      c.floor_accuracy = 0.0;
      c.floor_switch_latency_s = 0.0;
      int converged = 0, with_changes = 0;
      std::vector<double> coverages;
      SmoothnessSamples smooth;
      for (const auto& jr : job_results) {
        if (jr.scenario != si) continue;
        const Metrics& m = jr.per_preset[pi];
        ++c.runs;
        smooth.add(m);
        if (m.coverage95 >= 0.0) coverages.push_back(m.coverage95);
        c.mean_m += m.mean_m;
        c.cep50_m += m.cep50_m;
        c.cep95_m += m.cep95_m;
        c.availability += m.availability;
        c.compute_us_per_update += m.compute_us_per_update;
        c.floor_accuracy += m.floor_accuracy;
        if (m.floor_switch_latency_s >= 0.0) {
          c.floor_switch_latency_s += m.floor_switch_latency_s;
          ++with_changes;
        }
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
      c.floor_accuracy /= n;
      c.floor_switch_latency_s = with_changes ? c.floor_switch_latency_s / with_changes : -1.0;
      c.convergence_s = converged ? c.convergence_s / converged : -1.0;
      c.coverage95 = mean_or(coverages, -1.0);
      smooth.fill(c, false);
      cases.push_back(c);
    }
  }
  return cases;
}

std::vector<BenchmarkCase> run_trace_benchmark(const std::string& manifest_path, const BenchmarkOptions& opts) {
  std::ifstream in(manifest_path);
  if (!in) throw std::runtime_error("cannot open manifest " + manifest_path);
  const auto manifest = nlohmann::json::parse(in);
  const fs::path root = fs::path(manifest_path).parent_path();

  struct Entry {
    std::string group;
    std::string venue;
    std::string trace;
  };
  std::vector<Entry> entries;
  std::vector<std::string> groups;
  std::map<std::string, std::shared_ptr<const Venue>> venues;
  for (const auto& jt : manifest.at("traces")) {
    Entry e{jt.value("group", std::string("traces")), (root / jt.at("venue").get<std::string>()).string(),
            (root / jt.at("trace").get<std::string>()).string()};
    if (std::find(groups.begin(), groups.end(), e.group) == groups.end()) groups.push_back(e.group);
    if (!venues.count(e.venue)) venues[e.venue] = std::make_shared<Venue>(load_venue_file(e.venue));
    entries.push_back(std::move(e));
  }
  if (entries.empty()) throw std::runtime_error("no traces in " + manifest_path);

  const std::vector<EngineConfig> configs = preset_configs(opts);

  auto job = [&](std::size_t i) {
    const Venue& venue = *venues.at(entries[i].venue);
    const Trace trace = read_trace_file(entries[i].trace);
    std::vector<Metrics> per_preset;
    for (const auto& cfg : configs) per_preset.push_back(replay(venue, trace, cfg, opts.replay).metrics);
    return per_preset;
  };
  std::vector<std::vector<Metrics>> results(entries.size());
  if (opts.parallel) {
    std::vector<std::future<std::vector<Metrics>>> futures;
    for (std::size_t i = 0; i < entries.size(); ++i) futures.push_back(std::async(std::launch::async, job, i));
    for (std::size_t i = 0; i < entries.size(); ++i) results[i] = futures[i].get();
  } else {
    for (std::size_t i = 0; i < entries.size(); ++i) results[i] = job(i);
  }

  groups.push_back("all");
  std::vector<BenchmarkCase> cases;
  for (const auto& group : groups) {
    for (std::size_t pi = 0; pi < configs.size(); ++pi) {
      BenchmarkCase c;
      c.scenario = group;
      c.preset = opts.presets[pi];
      c.recorded = true;
      std::vector<double> means, cep50s, cep95s, coverages;
      SmoothnessSamples smooth;
      int converged = 0;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (group != "all" && entries[i].group != group) continue;
        const Metrics& m = results[i][pi];
        // A walk shorter than the warm-up has no scored samples.
        if (m.samples == 0) continue;
        ++c.runs;
        smooth.add(m);
        if (m.coverage95 >= 0.0) coverages.push_back(m.coverage95);
        means.push_back(m.mean_m);
        cep50s.push_back(m.cep50_m);
        cep95s.push_back(m.cep95_m);
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
      for (double v : means) c.mean_m += v / n;
      for (double v : cep50s) c.cep50_m += v / n;
      for (double v : cep95s) c.cep95_m += v / n;
      c.median_mean_m = percentile(means, 0.5);
      c.median_cep50_m = percentile(cep50s, 0.5);
      c.median_cep95_m = percentile(cep95s, 0.5);
      c.coverage95 = mean_or(coverages, -1.0);
      c.median_coverage95 = coverages.empty() ? -1.0 : percentile(coverages, 0.5);
      c.availability /= n;
      c.compute_us_per_update /= n;
      c.convergence_s = converged ? c.convergence_s / converged : -1.0;
      smooth.fill(c, true);
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
    if (c.multi_level && base.contains(c.scenario) && base[c.scenario].contains(c.preset) &&
        base[c.scenario][c.preset].contains("floor_accuracy")) {
      Finding f{Finding::Status::Ok, c.scenario, c.preset, "floor_acc", 0.0, c.floor_accuracy};
      f.baseline = base[c.scenario][c.preset].at("floor_accuracy").get<double>();
      if (c.floor_accuracy < f.baseline - tol.floor_accuracy) {
        f.status = Finding::Status::Regressed;
      } else if (c.floor_accuracy > f.baseline + tol.floor_accuracy) {
        f.status = Finding::Status::Improved;
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
    auto& entry = r[c.scenario][c.preset];
    entry = {{"mean_m", round3(c.mean_m)},
             {"cep50_m", round3(c.cep50_m)},
             {"cep95_m", round3(c.cep95_m)},
             {"availability", round3(c.availability)},
             {"convergence_s", round3(c.convergence_s)},
             {"unconverged_runs", c.unconverged_runs}};
    if (c.coverage95 >= 0.0) entry["coverage95"] = round3(c.coverage95);
    if (c.jitter_p50_m >= 0.0) entry["jitter_p50_m"] = round3(c.jitter_p50_m);
    if (c.jitter_p95_m >= 0.0) entry["jitter_p95_m"] = round3(c.jitter_p95_m);
    if (c.jumps_per_min >= 0.0) entry["jumps_per_min"] = round3(c.jumps_per_min);
    if (c.path_ratio >= 0.0) entry["path_ratio"] = round3(c.path_ratio);
    if (c.multi_level) {
      entry["floor_accuracy"] = round3(c.floor_accuracy);
      entry["floor_switch_latency_s"] = round3(c.floor_switch_latency_s);
      entry["level_flips"] = round3(c.level_flips);
      entry["level_changes"] = round3(c.level_changes);
    }
    if (c.recorded) {
      entry["traces"] = c.runs;
      entry["median_mean_m"] = round3(c.median_mean_m);
      entry["median_cep50_m"] = round3(c.median_cep50_m);
      entry["median_cep95_m"] = round3(c.median_cep95_m);
      if (c.median_coverage95 >= 0.0) entry["median_coverage95"] = round3(c.median_coverage95);
    }
  }
  return j.dump(indent);
}

std::string benchmark_to_markdown(const std::vector<BenchmarkCase>& results) {
  std::ostringstream out;
  const auto share = [](double v, char* buf, std::size_t size) {
    if (v < 0.0) {
      std::snprintf(buf, size, "-");
    } else {
      std::snprintf(buf, size, "%.0f%%", 100.0 * v);
    }
  };
  const auto number = [](double v, const char* fmt, char* buf, std::size_t size) {
    if (v < 0.0) {
      std::snprintf(buf, size, "-");
    } else {
      std::snprintf(buf, size, fmt, v);
    }
  };
  if (!results.empty() && results.front().recorded) {
    out << "| Group | Filter | Traces | Median CEP50 [m] | Median CEP95 [m] | Mean CEP50 [m] | Mean CEP95 [m] | "
           "Median coverage95 | Converged | Jitter p95 [m] | Jumps/min | Floor flips / changes | us/update |\n";
    out << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    char buf[384];
    for (const auto& c : results) {
      char coverage[16], jitter[16], jumps[16];
      share(c.median_coverage95, coverage, sizeof(coverage));
      number(c.jitter_p95_m, "%.2f", jitter, sizeof(jitter));
      number(c.jumps_per_min, "%.1f", jumps, sizeof(jumps));
      std::snprintf(buf, sizeof(buf),
                    "| %s | %s | %d | %.2f | %.2f | %.2f | %.2f | %s | %d/%d | %s | %s | %.1f / %.1f | %.1f |\n",
                    c.scenario.c_str(), c.preset.c_str(), c.runs, c.median_cep50_m, c.median_cep95_m, c.cep50_m,
                    c.cep95_m, coverage, c.runs - c.unconverged_runs, c.runs, jitter, jumps, c.level_flips,
                    c.level_changes, c.compute_us_per_update);
      out << buf;
    }
    return out.str();
  }
  out << "| Scenario | Filter | Mean [m] | CEP50 [m] | CEP95 [m] | Coverage95 | Converged in [s] | Floor ok | "
         "Floor switch [s] | Floor flips / changes | Jitter p95 [m] | Jumps/min | Path ratio | us/update |\n";
  out << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
  char buf[384];
  for (const auto& c : results) {
    char conv[32];
    if (c.convergence_s < 0.0) {
      std::snprintf(conv, sizeof(conv), "never");
    } else if (c.unconverged_runs > 0) {
      std::snprintf(conv, sizeof(conv), "%.1f (%d/%d)", c.convergence_s, c.runs - c.unconverged_runs, c.runs);
    } else {
      std::snprintf(conv, sizeof(conv), "%.1f", c.convergence_s);
    }
    char floor_ok[16] = "-", floor_switch[16] = "-", flips[32] = "-";
    if (c.multi_level) {
      std::snprintf(floor_ok, sizeof(floor_ok), "%.1f%%", 100.0 * c.floor_accuracy);
      std::snprintf(flips, sizeof(flips), "%.1f / %.1f", c.level_flips, c.level_changes);
      if (c.floor_switch_latency_s >= 0.0) {
        std::snprintf(floor_switch, sizeof(floor_switch), "%.1f", c.floor_switch_latency_s);
      }
    }
    char coverage[16], jitter[16], jumps[16], path[16];
    share(c.coverage95, coverage, sizeof(coverage));
    number(c.jitter_p95_m, "%.2f", jitter, sizeof(jitter));
    number(c.jumps_per_min, "%.1f", jumps, sizeof(jumps));
    number(c.path_ratio, "%.2f", path, sizeof(path));
    std::snprintf(buf, sizeof(buf), "| %s | %s | %.2f | %.2f | %.2f | %s | %s | %s | %s | %s | %s | %s | %s | %.1f |\n",
                  c.scenario.c_str(), c.preset.c_str(), c.mean_m, c.cep50_m, c.cep95_m, coverage, conv, floor_ok,
                  floor_switch, flips, jitter, jumps, path, c.compute_us_per_update);
    out << buf;
  }
  return out.str();
}

}  // namespace ipe::eval
