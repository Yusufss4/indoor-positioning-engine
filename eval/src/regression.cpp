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
      for (const auto& jr : job_results) {
        if (jr.scenario != si) continue;
        const Metrics& m = jr.per_preset[pi];
        ++c.runs;
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
      std::vector<double> means, cep50s, cep95s;
      int converged = 0;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (group != "all" && entries[i].group != group) continue;
        const Metrics& m = results[i][pi];
        // A walk shorter than the warm-up has no scored samples.
        if (m.samples == 0) continue;
        ++c.runs;
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
    if (c.multi_level) {
      entry["floor_accuracy"] = round3(c.floor_accuracy);
      entry["floor_switch_latency_s"] = round3(c.floor_switch_latency_s);
    }
    if (c.recorded) {
      entry["traces"] = c.runs;
      entry["median_mean_m"] = round3(c.median_mean_m);
      entry["median_cep50_m"] = round3(c.median_cep50_m);
      entry["median_cep95_m"] = round3(c.median_cep95_m);
    }
  }
  return j.dump(indent);
}

std::string benchmark_to_markdown(const std::vector<BenchmarkCase>& results) {
  std::ostringstream out;
  if (!results.empty() && results.front().recorded) {
    out << "| Group | Filter | Traces | Median CEP50 [m] | Median CEP95 [m] | Mean CEP50 [m] | Mean CEP95 [m] | "
           "Converged | us/update |\n";
    out << "|---|---|---:|---:|---:|---:|---:|---:|---:|\n";
    char buf[320];
    for (const auto& c : results) {
      std::snprintf(buf, sizeof(buf), "| %s | %s | %d | %.2f | %.2f | %.2f | %.2f | %d/%d | %.1f |\n",
                    c.scenario.c_str(), c.preset.c_str(), c.runs, c.median_cep50_m, c.median_cep95_m, c.cep50_m,
                    c.cep95_m, c.runs - c.unconverged_runs, c.runs, c.compute_us_per_update);
      out << buf;
    }
    return out.str();
  }
  out << "| Scenario | Filter | Mean [m] | CEP50 [m] | CEP95 [m] | Converged in [s] | Floor ok | Floor switch [s] | "
         "us/update |\n";
  out << "|---|---|---:|---:|---:|---:|---:|---:|---:|\n";
  char buf[320];
  for (const auto& c : results) {
    char conv[32];
    if (c.convergence_s < 0.0) {
      std::snprintf(conv, sizeof(conv), "never");
    } else if (c.unconverged_runs > 0) {
      std::snprintf(conv, sizeof(conv), "%.1f (%d/%d)", c.convergence_s, c.runs - c.unconverged_runs, c.runs);
    } else {
      std::snprintf(conv, sizeof(conv), "%.1f", c.convergence_s);
    }
    char floor_ok[16] = "-", floor_switch[16] = "-";
    if (c.multi_level) {
      std::snprintf(floor_ok, sizeof(floor_ok), "%.1f%%", 100.0 * c.floor_accuracy);
      if (c.floor_switch_latency_s >= 0.0) {
        std::snprintf(floor_switch, sizeof(floor_switch), "%.1f", c.floor_switch_latency_s);
      }
    }
    std::snprintf(buf, sizeof(buf), "| %s | %s | %.2f | %.2f | %.2f | %s | %s | %s | %.1f |\n", c.scenario.c_str(),
                  c.preset.c_str(), c.mean_m, c.cep50_m, c.cep95_m, conv, floor_ok, floor_switch,
                  c.compute_us_per_update);
    out << buf;
  }
  return out.str();
}

}  // namespace ipe::eval
