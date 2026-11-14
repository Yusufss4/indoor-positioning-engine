#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/config.hpp"
#include "ipe/eval.hpp"
#include "ipe/simulator.hpp"

namespace {
constexpr const char* kUsage = R"(usage:
  ipe-replay --venue VENUE.json --trace TRACE.csv [options]
  ipe-replay --scenario SCENARIO.json [--seed N] [options]

Runs a trace through the positioning engine and prints accuracy metrics as
JSON. With --scenario the trace is simulated in memory first.

options:
  --filter NAME          grid, the only filter (kept for existing scripts)
  --config FILE          JSON merge patch applied to the default config. Without it, the config
                         recommended for the venue is used: the Wi-Fi fingerprint settings when
                         the venue has training scans
  --set JSON             inline JSON merge patch, e.g. '{"filter":{"heading_bins":72}}'
  --warmup SECONDS       exclude the first seconds from error stats (default 5)
  --out FILE             per-truth-sample estimates CSV
  --track FILE           the estimate every 0.2 s of trace time, CSV (what the smoothness metrics use)
  --events FILE          room enter/exit events CSV, estimated and true (venues with rooms)
  --metrics FILE         write metrics JSON to a file as well
  --cells FILE           snapshots of the posterior's heaviest cells, CSV
  --cells-every N        snapshot every N truth samples (default 5)
  --print-config         print the effective engine config and exit (with a venue or scenario:
                         the config that run would use)
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

    ipe::eval::check_filter_name(args.get("filter", ipe::eval::kFilterName));
    // The config: a --config file on the engine's defaults, or else what the venue calls for; then --set.
    const auto config_for = [&](const ipe::Venue* venue) {
      ipe::EngineConfig cfg;
      if (args.has("config")) {
        ipe::apply_config_patch(cfg, read_file(args.get("config")));
      } else if (venue != nullptr) {
        cfg = ipe::recommended_config(*venue);
      }
      if (args.has("set")) ipe::apply_config_patch(cfg, args.get("set"));
      return cfg;
    };
    const bool print_config = args.has("print-config");
    if (print_config && !args.has("scenario") && !args.has("venue")) {
      std::cout << ipe::config_to_json(config_for(nullptr)) << "\n";
      return 0;
    }

    ipe::Venue venue;
    ipe::Trace trace;
    if (args.has("scenario")) {
      const auto sc = ipe::sim::load_scenario_file(args.get("scenario"));
      venue = ipe::load_venue_file(args.has("venue") ? args.get("venue") : sc.venue_path);
      std::optional<std::uint64_t> seed;
      if (args.has("seed")) seed = std::stoull(args.get("seed"));
      if (!print_config) trace = ipe::sim::simulate(sc, venue, seed);
    } else {
      venue = ipe::load_venue_file(args.require("venue"));
      if (!print_config) trace = ipe::read_trace_file(args.require("trace"));
    }
    const ipe::EngineConfig cfg = config_for(&venue);
    if (print_config) {
      std::cout << ipe::config_to_json(cfg) << "\n";
      return 0;
    }

    ipe::eval::ReplayOptions opts;
    opts.warmup_s = std::stod(args.get("warmup", "5"));
    if (args.has("cells")) opts.snapshot_every = std::stoi(args.get("cells-every", "5"));

    const auto result = ipe::eval::replay(venue, trace, cfg, opts);
    if (args.has("out")) ipe::eval::write_estimates_csv(args.get("out"), result);
    if (args.has("track")) ipe::eval::write_track_csv(args.get("track"), result);
    if (args.has("cells")) ipe::eval::write_cells_csv(args.get("cells"), result);
    if (args.has("events")) ipe::eval::write_room_events_csv(args.get("events"), venue, result);

    auto metrics = nlohmann::json::parse(ipe::eval::metrics_to_json(result.metrics));
    metrics["filter"] = args.get("filter", "grid");
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
