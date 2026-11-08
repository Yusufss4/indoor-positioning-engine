#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/plan.hpp"
#include "ipe/simulator.hpp"

namespace {
constexpr const char* kUsage = R"(usage:
  ipe-plan analyze (--venue FILE | --scenario FILE) [options]

Scores the venue's beacon layout from the map and the RSSI model alone: how
many beacons a phone hears, the Cramer-Rao bound on its position error from
one packet per beacon and the floor margin, on a grid over every level. With
--scenario the same scores are also computed along the scenario's walk. Prints
a JSON summary; bounds that are infinite (the heard beacons cannot fix a
position) are written as null.

options:
  --grid M            grid and path spacing (default 1)
  --sigma DB          RSSI spread per packet (default 5)
  --min-rssi DBM      beacons expected below this are not heard (default -85)
  --offset-sigma DB   prior spread of the device offset; 0 = known (default 8)
  --target M          share_within_target counts points with a bound <= M (default 2)
  --window M          along the walk, pool what was heard over the last M metres (default 10)
  --no-walls          ignore wall loss when deciding what is heard
  --out FILE          per-point CSV of the venue grid: level,x,y,heard,crlb_m,floor_margin_db
  --json FILE         also write the summary here
)";

double rounded(double v) { return std::isfinite(v) ? std::round(v * 1000.0) / 1000.0 : v; }

nlohmann::json summary_json(const ipe::plan::Summary& s, bool multi_level) {
  nlohmann::json j = {{"points", s.points},
                      {"heard_mean", rounded(s.heard_mean)},
                      {"share_heard_below_3", rounded(s.share_heard_below_3)},
                      {"crlb_p50_m", rounded(s.crlb_p50_m)},
                      {"crlb_p95_m", rounded(s.crlb_p95_m)},
                      {"share_within_target", rounded(s.share_within_target)}};
  if (multi_level) {
    j["floor_margin_p05_db"] = rounded(s.floor_margin_p05_db);
    j["share_floor_margin_negative"] = rounded(s.share_floor_margin_negative);
  }
  return j;
}

void write_csv(const std::string& path, const std::vector<ipe::plan::PointScore>& scores) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write " + path);
  out << "level,x,y,heard,crlb_m,floor_margin_db\n";
  for (const auto& p : scores) {
    out << p.at.level << ',' << p.at.pos.x << ',' << p.at.pos.y << ',' << p.heard << ',' << rounded(p.crlb_m) << ','
        << rounded(p.floor_margin_db) << '\n';
  }
}

int analyze(const ipe::cli::Args& args) {
  ipe::plan::AnalysisConfig cfg;
  if (args.has("grid")) cfg.grid_m = std::stod(args.get("grid"));
  if (args.has("sigma")) cfg.sigma_db = std::stod(args.get("sigma"));
  if (args.has("min-rssi")) cfg.min_rssi_dbm = std::stod(args.get("min-rssi"));
  if (args.has("offset-sigma")) cfg.offset_sigma_db = std::stod(args.get("offset-sigma"));
  if (args.has("target")) cfg.target_m = std::stod(args.get("target"));
  if (args.has("window")) cfg.window_m = std::stod(args.get("window"));
  cfg.wall_aware = !args.has("no-walls");

  std::optional<ipe::sim::Scenario> scenario;
  std::string venue_path;
  if (args.has("scenario")) {
    scenario = ipe::sim::load_scenario_file(args.get("scenario"));
    venue_path = scenario->venue_path;
  } else {
    venue_path = args.require("venue");
  }
  const auto venue = ipe::load_venue_file(venue_path);

  const auto grid = ipe::plan::venue_grid(venue, cfg.grid_m);
  const auto grid_scores = ipe::plan::score_points(venue, grid, cfg);
  nlohmann::json out = {{"venue", venue.name()},
                        {"venue_path", venue_path},
                        {"beacons", venue.beacons().size()},
                        {"config",
                         {{"grid_m", cfg.grid_m},
                          {"sigma_db", cfg.sigma_db},
                          {"min_rssi_dbm", cfg.min_rssi_dbm},
                          {"offset_sigma_db", cfg.offset_sigma_db},
                          {"target_m", cfg.target_m},
                          {"window_m", cfg.window_m},
                          {"wall_aware", cfg.wall_aware}}},
                        {"grid", summary_json(ipe::plan::summarize(grid_scores, cfg), venue.multi_level())}};

  if (scenario) {
    std::vector<ipe::plan::Location> waypoints;
    for (const auto& w : scenario->walk.waypoints) {
      const auto level = venue.level_index(w.level);
      if (!level) throw std::invalid_argument("waypoint on unknown level " + std::to_string(w.level));
      waypoints.push_back({w.pos, *level});
    }
    const auto path = ipe::plan::sample_path(waypoints, cfg.grid_m);
    const auto path_scores = ipe::plan::score_path(venue, path, cfg);
    out["scenario"] = scenario->name;
    out["path"] = summary_json(ipe::plan::summarize(path_scores, cfg), venue.multi_level());
  }

  if (args.has("out")) write_csv(args.get("out"), grid_scores);
  const std::string text = out.dump(2) + "\n";
  if (args.has("json")) {
    std::ofstream f(args.get("json"));
    if (!f) throw std::runtime_error("cannot write " + args.get("json"));
    f << text;
  }
  std::cout << text;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "--help" || command == "help") {
      std::cout << kUsage;
      return 0;
    }
    const ipe::cli::Args args(argc - 1, argv + 1, {"help", "no-walls"});
    if (command == "analyze") return analyze(args);
    throw std::invalid_argument(command.empty() ? "missing command" : "unknown command: " + command);
  });
}
