#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/eval.hpp"
#include "ipe/simulator.hpp"

namespace {
constexpr const char* kUsage = R"(usage:
  ipe-replay --venue VENUE.json --trace TRACE.csv [options]
  ipe-replay --scenario SCENARIO.json [--seed N] [options]

Runs a trace through the positioning engine and prints accuracy metrics as
JSON. With --scenario the trace is simulated in memory first.

options:
  --filter NAME          pf (default), pf-nomap, ekf, wcl
  --config FILE          JSON merge patch applied to the preset config
  --set JSON             inline JSON merge patch, e.g. '{"pf":{"num_particles":500}}'
  --warmup SECONDS       exclude the first seconds from error stats (default 5)
  --out FILE             per-truth-sample estimates CSV
  --events FILE          room enter/exit events CSV, estimated and true (venues with rooms)
  --metrics FILE         write metrics JSON to a file as well
  --particles FILE       particle snapshots CSV (particle filter only)
  --particles-every N    snapshot every N truth samples (default 5)
  --print-config         print the effective engine config and exit
)";

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
}  // namespace

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    const ipe::cli::Args args(argc, argv, {"help", "print-config"});
    if (args.has("help")) {
      std::cout << kUsage;
      return 0;
    }

    auto cfg = ipe::eval::preset_config(args.get("filter", "pf"));
    if (args.has("config")) ipe::eval::apply_config_overrides(cfg, read_file(args.get("config")));
    if (args.has("set")) ipe::eval::apply_config_overrides(cfg, args.get("set"));
    if (args.has("print-config")) {
      std::cout << ipe::eval::config_to_json(cfg) << "\n";
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

    ipe::eval::ReplayOptions opts;
    opts.warmup_s = std::stod(args.get("warmup", "5"));
    if (args.has("particles")) opts.particle_snapshot_every = std::stoi(args.get("particles-every", "5"));

    const auto result = ipe::eval::replay(venue, trace, cfg, opts);
    if (args.has("out")) ipe::eval::write_estimates_csv(args.get("out"), result);
    if (args.has("particles")) ipe::eval::write_particles_csv(args.get("particles"), result);
    if (args.has("events")) ipe::eval::write_room_events_csv(args.get("events"), venue, result);

    auto metrics = nlohmann::json::parse(ipe::eval::metrics_to_json(result.metrics));
    metrics["filter"] = args.get("filter", "pf");
    metrics["venue"] = venue.name();
    if (auto it = trace.metadata.find("scenario"); it != trace.metadata.end()) metrics["scenario"] = it->second;
    const std::string text = metrics.dump(2);
    if (args.has("metrics")) {
      std::ofstream(args.get("metrics")) << text << "\n";
    }
    std::cout << text << "\n";
    return 0;
  });
}
