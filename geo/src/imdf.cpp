#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>

#include "geo_internal.hpp"
#include "ipe/geo.hpp"

namespace ipe::geo {

using namespace detail;
namespace fs = std::filesystem;

namespace {

/// Deterministic UUID-shaped id, so re-exporting a venue gives identical files.
std::string make_uuid(const std::string& key) {
  const auto fnv = [&](std::uint64_t h) {
    for (unsigned char c : key) {
      h ^= c;
      h *= 0x100000001b3ULL;
    }
    return h;
  };
  const std::uint64_t a = fnv(0xcbf29ce484222325ULL);
  const std::uint64_t b = fnv(0x84222325cbf29ce4ULL);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-5%03x-8%03x-%012llx", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xffff), static_cast<unsigned>(a & 0xfff),
                static_cast<unsigned>((b >> 48) & 0xfff), static_cast<unsigned long long>(b & 0xffffffffffffULL));
  return buf;
}

ojson imdf_feature(const std::string& id, const char* feature_type, ojson geometry, ojson properties) {
  return ojson{{"id", id},
               {"type", "Feature"},
               {"feature_type", feature_type},
               {"geometry", std::move(geometry)},
               {"properties", std::move(properties)}};
}

ojson label(const std::string& s) { return ojson{{"en", s}}; }

const std::set<std::string> kWallCategories{"structure", "concrete", "brick", "drywall", "glass", "wood", "column"};
const std::set<std::string> kOpenAreaCategories{
    "walkway",  "unenclosedarea", "opentobelow", "lobby",      "footbridge", "movingwalkway", "ramp",
    "platform", "fieldofplay",    "terrace",     "vegetation", "parking",    "steps"};
const std::set<std::string> kConnectorCategories{"stairs", "elevator", "escalator"};

json read_collection(const fs::path& p) {
  const json j = json::parse(read_file(p.string()));
  if (!j.contains("features")) throw std::runtime_error("imdf: " + p.string() + " is not a FeatureCollection");
  return j.at("features");
}

std::string label_text(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_object() && !v.empty()) {
    if (v.contains("en") && v.at("en").is_string()) return v.at("en").get<std::string>();
    if (v.begin()->is_string()) return v.begin()->get<std::string>();
  }
  return "";
}

}  // namespace

void export_imdf(const Venue& v, const std::string& dir, const ExportOptions& opts) {
  if (!v.georef()) throw std::runtime_error("venue " + v.name() + " has no georef; add one to export it");
  const GeoRef& g = *v.georef();
  const fs::path out(dir);
  const Bounds& b = v.bounds();
  const std::vector<Vec2> outline{{b.min_x, b.min_y}, {b.max_x, b.min_y}, {b.max_x, b.max_y}, {b.min_x, b.max_y}};
  const ojson center{{"type", "Point"},
                     {"coordinates", position(g, {0.5 * (b.min_x + b.max_x), 0.5 * (b.min_y + b.max_y)})}};
  const auto uuid = [&](const std::string& key) { return make_uuid(v.name() + "/" + key); };

  // "created" is fixed so that re-exports are byte-identical.
  const ojson manifest{
      {"version", "1.0.0"}, {"created", "2026-01-01T00:00:00Z"}, {"generated_by", "ipe-import"}, {"language", "en"}};
  write_file((out / "manifest.json").string(), manifest.dump(2) + "\n");

  write_file((out / "venue.geojson").string(),
             feature_collection({imdf_feature(
                 uuid("venue"), "venue", polygon(g, outline),
                 {{"category", "unspecified"},
                  {"restriction", nullptr},
                  {"name", label(v.name())},
                  {"alt_name", nullptr},
                  {"hours", nullptr},
                  {"phone", nullptr},
                  {"website", nullptr},
                  {"display_point", center},
                  {"address_id", nullptr},
                  {"ipe:georef",
                   {{"origin_lat", g.origin_lat}, {"origin_lon", g.origin_lon}, {"rotation_deg", g.rotation_deg}}},
                  {"ipe:wall_attenuation_db", v.wall_attenuation_db()},
                  {"ipe:floor_attenuation_db", v.floor_attenuation_db()}})}));

  std::vector<ojson> levels;
  for (const Level& l : v.levels()) {
    levels.push_back(imdf_feature(uuid("level/" + std::to_string(l.ordinal)), "level", polygon(g, outline),
                                  {{"category", "unspecified"},
                                   {"restriction", nullptr},
                                   {"outdoor", false},
                                   {"ordinal", l.ordinal},
                                   {"name", label(l.name)},
                                   {"short_name", label(std::to_string(l.ordinal))},
                                   {"display_point", center},
                                   {"address_id", nullptr},
                                   {"building_ids", nullptr},
                                   {"ipe:elevation_m", l.elevation_m},
                                   {"ipe:height_m", l.height_m}}));
  }
  write_file((out / "level.geojson").string(), feature_collection(levels));

  const auto unit_props = [](const std::string& category, const std::string& level_id, ojson name) {
    return ojson{{"category", category},    {"restriction", nullptr}, {"accessibility", nullptr},
                 {"name", std::move(name)}, {"alt_name", nullptr},    {"display_point", nullptr},
                 {"level_id", level_id}};
  };
  std::vector<ojson> units;
  for (const Level& l : v.levels()) {
    const std::string level_id = uuid("level/" + std::to_string(l.ordinal));
    for (std::size_t k = 0; k < l.walls.size(); ++k) {
      units.push_back(imdf_feature(uuid("wall/" + std::to_string(l.ordinal) + "/" + std::to_string(k)), "unit",
                                   polygon(g, wall_rect(l.walls[k], opts.wall_thickness_m)),
                                   unit_props("structure", level_id, nullptr)));
    }
  }
  for (const Connector& c : v.connectors()) {
    for (int i : c.levels) {
      const Level& l = v.level(i);
      ojson props = unit_props(std::string(to_string(c.kind)), uuid("level/" + std::to_string(l.ordinal)), label(c.id));
      props["ipe:connector_id"] = c.id;
      units.push_back(
          imdf_feature(uuid("connector/" + c.id + "/" + std::to_string(l.ordinal)), "unit", polygon(g, c.area), props));
    }
  }
  write_file((out / "unit.geojson").string(), feature_collection(units));
  write_file((out / "opening.geojson").string(), feature_collection({}));

  // Not part of IMDF: beacons live in a sidecar the importer picks up.
  std::string beacons = "{\"beacons\": [\n";
  for (std::size_t k = 0; k < v.beacons().size(); ++k) {
    const Beacon& bc = v.beacons()[k];
    const LatLon ll = g.to_wgs84(bc.pos);
    beacons += "  " +
               ojson{{"id", bc.id},
                     {"lat", round_deg(ll.lat)},
                     {"lon", round_deg(ll.lon)},
                     {"level", v.level(bc.level).ordinal},
                     {"tx_power", bc.tx_power_dbm},
                     {"path_loss_exp", bc.path_loss_exp}}
                   .dump() +
               (k + 1 < v.beacons().size() ? ",\n" : "\n");
  }
  beacons += "]}\n";
  write_file((out / "beacons.json").string(), beacons);
}

ImportResult import_imdf(const std::string& dir, const ImportOptions& opts) {
  const fs::path in(dir);
  if (!fs::exists(in / "level.geojson")) throw std::runtime_error("imdf: " + dir + " has no level.geojson");

  json features = json::array();
  std::vector<std::string> warnings;

  if (fs::exists(in / "venue.geojson")) {
    for (const auto& f : read_collection(in / "venue.geojson")) {
      const json& p = f.at("properties");
      json props{{"feature_type", "venue"}, {"name", label_text(p.value("name", json()))}};
      if (p.contains("ipe:georef")) {
        const json& gr = p.at("ipe:georef");
        props["origin_lat"] = gr.at("origin_lat");
        props["origin_lon"] = gr.at("origin_lon");
        props["rotation_deg"] = gr.value("rotation_deg", 0.0);
      }
      if (p.contains("ipe:wall_attenuation_db")) props["wall_attenuation_db"] = p.at("ipe:wall_attenuation_db");
      if (p.contains("ipe:floor_attenuation_db")) props["floor_attenuation_db"] = p.at("ipe:floor_attenuation_db");
      features.push_back({{"type", "Feature"}, {"properties", props}, {"geometry", f.at("geometry")}});
      break;
    }
  }

  std::map<std::string, int> ordinal_of;
  for (const auto& f : read_collection(in / "level.geojson")) {
    const json& p = f.at("properties");
    const int ordinal = p.at("ordinal").get<int>();
    ordinal_of[f.at("id").get<std::string>()] = ordinal;
    json props{{"feature_type", "level"}, {"ordinal", ordinal}};
    if (p.contains("name") && !p.at("name").is_null()) props["name"] = label_text(p.at("name"));
    if (p.contains("ipe:elevation_m")) props["elevation_m"] = p.at("ipe:elevation_m");
    if (p.contains("ipe:height_m")) props["height_m"] = p.at("ipe:height_m");
    features.push_back({{"type", "Feature"}, {"properties", props}, {"geometry", f.at("geometry")}});
  }

  const auto level_of = [&](const json& p, const char* what) -> std::optional<int> {
    const auto it = ordinal_of.find(p.value("level_id", ""));
    if (it == ordinal_of.end()) {
      warnings.push_back(std::string("imdf: skipped a ") + what + " with an unknown level_id");
      return std::nullopt;
    }
    return it->second;
  };

  std::map<std::string, int> skipped_categories;
  if (fs::exists(in / "unit.geojson")) {
    for (const auto& f : read_collection(in / "unit.geojson")) {
      const json& p = f.at("properties");
      const std::string category = p.value("category", "unspecified");
      std::string type;
      if (kWallCategories.count(category)) {
        type = "wall";
      } else if (kConnectorCategories.count(category)) {
        type = category;
      } else if (kOpenAreaCategories.count(category)) {
        ++skipped_categories[category];
        continue;
      } else {
        type = "room";
      }
      const auto ordinal = level_of(p, "unit");
      if (!ordinal) continue;
      json props{{"feature_type", type}, {"level", *ordinal}};
      if (type != "wall" && type != "room") {
        props["id"] = p.contains("ipe:connector_id") ? p.at("ipe:connector_id") : f.at("id");
      }
      features.push_back({{"type", "Feature"}, {"properties", props}, {"geometry", f.at("geometry")}});
    }
  }
  for (const auto& [category, count] : skipped_categories) {
    warnings.push_back("imdf: " + std::to_string(count) + " open-area unit(s) of category '" + category +
                       "' contribute no walls");
  }

  if (fs::exists(in / "opening.geojson")) {
    for (const auto& f : read_collection(in / "opening.geojson")) {
      const auto ordinal = level_of(f.at("properties"), "opening");
      if (!ordinal) continue;
      features.push_back({{"type", "Feature"},
                          {"properties", {{"feature_type", "opening"}, {"level", *ordinal}}},
                          {"geometry", f.at("geometry")}});
    }
  }

  ImportOptions o = opts;
  if (o.beacons_json.empty() && fs::exists(in / "beacons.json"))
    o.beacons_json = read_file((in / "beacons.json").string());

  const json fc{{"type", "FeatureCollection"}, {"features", features}};
  ImportResult r = import_geojson(fc.dump(), Mapping{}, o);
  r.warnings.insert(r.warnings.begin(), warnings.begin(), warnings.end());
  return r;
}

}  // namespace ipe::geo
