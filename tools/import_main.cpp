#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "cli.hpp"
#include "ipe/geo.hpp"

namespace {
constexpr const char* kUsage = R"(usage:
  ipe-import imdf DIR [--out VENUE.json] [import options]
  ipe-import geojson FILE.geojson [--mapping MAPPING.json] [--out VENUE.json] [import options]
  ipe-import export VENUE.json [--out FILE.geojson] [--wall-thickness M]
  ipe-import export-imdf VENUE.json --out DIR [--wall-thickness M]

Converts between the venue JSON used by the engine and map formats of indoor
mapping platforms. "imdf" reads an IMDF directory, "geojson" reads any GeoJSON
FeatureCollection through a mapping table (default: the "ipe" scheme written
by "export"; see data/mappings/). Output goes to stdout without --out.
Warnings go to stderr.

import options:
  --name NAME              venue name
  --beacons FILE           beacon sidecar (list of {id, lat, lon | x, y, level})
  --beacon-grid M          place beacons on an M metre grid if the map has none
  --level-height M         floor-to-floor height when levels have no elevation (default 4)
  --wall-attenuation DB    override the per-wall radio loss
  --floor-attenuation DB   override the per-slab radio loss
  --min-wall M             drop wall pieces shorter than M after cutting doors (default 0.01)
  --origin LAT,LON[,ROT]   anchor the local frame here instead of the data's georef
)";

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void emit(const ipe::cli::Args& args, const std::string& text) {
  if (!args.has("out")) {
    std::cout << text;
    return;
  }
  std::ofstream out(args.get("out"), std::ios::binary);
  if (!out) throw std::runtime_error("cannot write " + args.get("out"));
  out << text;
}

ipe::geo::ImportOptions import_options(const ipe::cli::Args& args) {
  ipe::geo::ImportOptions o;
  o.name = args.get("name");
  if (args.has("beacons")) o.beacons_json = read_text(args.get("beacons"));
  if (args.has("beacon-grid")) o.beacon_grid_m = std::stod(args.get("beacon-grid"));
  if (args.has("level-height")) o.level_height_m = std::stod(args.get("level-height"));
  if (args.has("wall-attenuation")) o.wall_attenuation_db = std::stod(args.get("wall-attenuation"));
  if (args.has("floor-attenuation")) o.floor_attenuation_db = std::stod(args.get("floor-attenuation"));
  if (args.has("min-wall")) o.min_wall_m = std::stod(args.get("min-wall"));
  if (args.has("origin")) {
    std::stringstream ss(args.get("origin"));
    std::string part;
    std::vector<double> v;
    while (std::getline(ss, part, ',')) v.push_back(std::stod(part));
    if (v.size() < 2 || v.size() > 3) throw std::invalid_argument("--origin expects LAT,LON[,ROT]");
    o.georef = ipe::GeoRef{v[0], v[1], v.size() == 3 ? v[2] : 0.0};
  }
  return o;
}

ipe::geo::ExportOptions export_options(const ipe::cli::Args& args) {
  ipe::geo::ExportOptions o;
  if (args.has("wall-thickness")) o.wall_thickness_m = std::stod(args.get("wall-thickness"));
  return o;
}

void report(const ipe::geo::ImportResult& r) {
  for (const auto& w : r.warnings) std::cerr << "warning: " << w << "\n";
  const auto& v = r.venue;
  std::size_t walls = 0;
  for (const auto& l : v.levels()) walls += l.walls.size();
  std::cerr << "imported " << v.name() << ": " << v.level_count() << " level(s), " << walls << " walls, "
            << v.connectors().size() << " connector(s), " << v.beacons().size() << " beacon(s)\n";
}

}  // namespace

int main(int argc, char** argv) {
  return ipe::cli::run_main(kUsage, [&] {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
      std::cout << kUsage;
      return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (argc < 3 || std::string(argv[2]).rfind("--", 0) == 0) {
      throw std::invalid_argument(command + ": missing input path");
    }
    const std::string input = argv[2];
    // Args skips its argv[0]; hand it the command line after the input path.
    const ipe::cli::Args args(argc - 2, argv + 2, {});

    if (command == "imdf") {
      const auto r = ipe::geo::import_imdf(input, import_options(args));
      report(r);
      emit(args, ipe::geo::venue_to_json(r.venue));
    } else if (command == "geojson") {
      const auto mapping = args.has("mapping") ? ipe::geo::load_mapping_file(args.get("mapping")) : ipe::geo::Mapping{};
      const auto r = ipe::geo::import_geojson(read_text(input), mapping, import_options(args));
      report(r);
      emit(args, ipe::geo::venue_to_json(r.venue));
    } else if (command == "export") {
      emit(args, ipe::geo::export_geojson(ipe::load_venue_file(input), export_options(args)));
    } else if (command == "export-imdf") {
      const std::string dir = args.require("out");
      std::filesystem::create_directories(dir);
      ipe::geo::export_imdf(ipe::load_venue_file(input), dir, export_options(args));
    } else {
      throw std::invalid_argument("unknown command: " + command);
    }
    return 0;
  });
}
