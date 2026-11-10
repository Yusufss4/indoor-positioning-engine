// ipe-grid: the grid filter prototype (docs/grid.md) next to the particle filter.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/eval.hpp"
#include "ipe/grid_engine.hpp"
#include "ipe/regression.hpp"
#include "ipe/simulator.hpp"

namespace {
constexpr const char* kUsage = R"(usage:
  ipe-grid replay (--venue VENUE.json --trace TRACE.csv | --scenario SCENARIO.json [--seed N]) [options]
  ipe-grid compare (--scenarios DIR [--seeds N] | --traces MANIFEST.json) [options]

replay runs one trace through the grid filter and prints its metrics as JSON, like ipe-replay.
compare replays every trace through the particle filter (preset pf) and the grid filter and prints
the ipe-regress table; nothing is compared with a baseline.

options:
  --config FILE      JSON merge patch on the engine config (pdr, rssi, baro, smoothing, rooms, pf)
  --set JSON         inline merge patch on the engine config, after --config
  --grid-set JSON    merge patch on the grid filter config, e.g. '{"heading_bins":72}'
  --print-config     print the effective grid filter config and exit
  --warmup SECONDS   exclude the first seconds from error stats (default 5)
replay:
  --out FILE         per-truth-sample estimates CSV
  --track FILE       the estimate every 0.2 s of trace time, CSV
  --events FILE      room enter/exit events CSV
  --metrics FILE     metrics JSON
  --particles FILE   snapshots of the heaviest cells, CSV
  --particles-every N  snapshot every N truth samples (default 5)
compare:
  --seeds N          runs per scenario (default 3)
  --variants JSON    {"name": grid patch, ...}: one grid column per variant, after --grid-set
  --no-pf            leave the particle filter out
  --serial           one trace at a time
  --markdown FILE    write the table
  --json FILE        write the results as JSON
)";

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// Builds each venue's GridMap once per grid config and shares it between threads.
class MapCache {
 public:
  std::shared_ptr<const ipe::GridMap> get(const ipe::Venue& venue, const ipe::GridFilterConfig& cfg) {
    const auto key = std::make_pair(&venue, ipe::grid::config_to_json(cfg, -1));
    std::shared_future<std::shared_ptr<const ipe::GridMap>> fut;
    std::promise<std::shared_ptr<const ipe::GridMap>> promise;
    bool build = false;
    {
      const std::lock_guard lock(mutex_);
      auto it = maps_.find(key);
      if (it == maps_.end()) {
        fut = promise.get_future().share();
        maps_.emplace(key, fut);
        build = true;
      } else {
        fut = it->second;
      }
    }
    if (build) {
      try {
        promise.set_value(std::make_shared<const ipe::GridMap>(venue, cfg));
      } catch (...) {
        promise.set_exception(std::current_exception());
      }
    }
    return fut.get();
  }

 private:
  std::mutex mutex_;
  std::map<std::pair<const ipe::Venue*, std::string>, std::shared_future<std::shared_ptr<const ipe::GridMap>>> maps_;
};

ipe::EngineConfig engine_config(const ipe::cli::Args& args) {
  auto cfg = ipe::eval::preset_config("pf");
  if (args.has("config")) ipe::eval::apply_config_overrides(cfg, read_file(args.get("config")));
  if (args.has("set")) ipe::eval::apply_config_overrides(cfg, args.get("set"));
  return cfg;
}

ipe::GridFilterConfig grid_config(const ipe::cli::Args& args) {
  ipe::GridFilterConfig cfg;
  if (args.has("grid-set")) ipe::grid::apply_overrides(cfg, args.get("grid-set"));
  return cfg;
}

int run_replay(const ipe::cli::Args& args) {
  const auto base = engine_config(args);
  const auto gcfg = grid_config(args);
  if (args.has("print-config")) {
    std::cout << ipe::grid::config_to_json(gcfg) << "\n";
    return 0;
  }
  ipe::Venue venue;
  ipe::Trace trace;
  if (args.has("scenario")) {
    const auto sc = ipe::sim::load_scenario_file(args.get("scenario"));
    venue = ipe::load_venue_file(args.has("venue") ? args.get("venue") : sc.venue_path);
    std::optional<std::uint64_t> seed;
    if (args.has("seed")) seed = std::stoull(args.get("seed"));
    trace = ipe::sim::simulate(sc, venue, seed);
  } else {
    venue = ipe::load_venue_file(args.require("venue"));
    trace = ipe::read_trace_file(args.require("trace"));
  }

  const auto t0 = std::chrono::steady_clock::now();
  const auto map = std::make_shared<const ipe::GridMap>(venue, gcfg);
  const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  ipe::eval::ReplayOptions opts;
  opts.warmup_s = std::stod(args.get("warmup", "5"));
  if (args.has("particles")) opts.particle_snapshot_every = std::stoi(args.get("particles-every", "5"));
  ipe::GridEngine engine(venue, map, base, gcfg);
  const auto result = ipe::eval::replay_with(engine, venue, trace, opts);
  if (args.has("out")) ipe::eval::write_estimates_csv(args.get("out"), result);
  if (args.has("track")) ipe::eval::write_track_csv(args.get("track"), result);
  if (args.has("particles")) ipe::eval::write_particles_csv(args.get("particles"), result);
  if (args.has("events")) ipe::eval::write_room_events_csv(args.get("events"), venue, result);

  auto metrics = nlohmann::json::parse(ipe::eval::metrics_to_json(result.metrics));
  metrics["filter"] = "grid";
  metrics["venue"] = venue.name();
  if (auto it = trace.metadata.find("scenario"); it != trace.metadata.end()) metrics["scenario"] = it->second;
  metrics["grid_cells"] = map->size();
  metrics["grid_cell_m"] = map->cell_m();
  metrics["grid_map_build_s"] = build_s;
  const std::string text = metrics.dump(2);
  if (args.has("metrics")) std::ofstream(args.get("metrics")) << text << "\n";
  std::cout << text << "\n";
  return 0;
}

int run_compare(const ipe::cli::Args& args) {
  ipe::eval::BenchmarkOptions opts;
  opts.seeds = std::stoi(args.get("seeds", "3"));
  opts.parallel = !args.has("serial");
  opts.replay.warmup_s = std::stod(args.get("warmup", "5"));
  if (args.has("config")) opts.config_patches.push_back(read_file(args.get("config")));
  if (args.has("set")) opts.config_patches.push_back(args.get("set"));
  if (args.has("no-pf")) opts.presets.clear();
  if (args.has("traces")) opts.seeds = 1;

  const ipe::EngineConfig base = engine_config(args);
  const ipe::GridFilterConfig gcfg = grid_config(args);
  std::vector<std::pair<std::string, ipe::GridFilterConfig>> variants;
  if (args.has("variants")) {
    const auto j = nlohmann::json::parse(args.get("variants"));
    for (const auto& [name, patch] : j.items()) {
      ipe::GridFilterConfig v = gcfg;
      ipe::grid::apply_overrides(v, patch.dump());
      variants.emplace_back("grid:" + name, v);
    }
  } else {
    variants.emplace_back("grid", gcfg);
  }
  auto cache = std::make_shared<MapCache>();
  for (const auto& [name, vcfg] : variants) {
    opts.estimators.push_back({name, [cache, base, vcfg](const ipe::Venue& venue, const ipe::Trace& trace,
                                                         const ipe::eval::ReplayOptions& ro) {
                                 ipe::GridEngine engine(venue, cache->get(venue, vcfg), base, vcfg);
                                 return ipe::eval::replay_with(engine, venue, trace, ro);
                               }});
  }

  const auto results = args.has("traces") ? ipe::eval::run_trace_benchmark(args.get("traces"), opts)
                                          : ipe::eval::run_benchmark(args.require("scenarios"), opts);
  const std::string table = ipe::eval::benchmark_to_markdown(results);
  std::cout << table << "\n";
  if (args.has("markdown")) std::ofstream(args.get("markdown")) << table;
  if (args.has("json")) std::ofstream(args.get("json")) << ipe::eval::benchmark_to_json(results, opts) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    if (argc < 2) throw std::invalid_argument("missing command (replay or compare)");
    const std::string command = argv[1];
    if (command == "--help" || command == "help") {
      std::cout << kUsage;
      return 0;
    }
    const ipe::cli::Args args(argc - 1, argv + 1, {"help", "print-config", "no-pf", "serial"});
    if (args.has("help")) {
      std::cout << kUsage;
      return 0;
    }
    if (command == "replay") return run_replay(args);
    if (command == "compare") return run_compare(args);
    throw std::invalid_argument("unknown command '" + command + "' (expected replay or compare)");
  });
}
