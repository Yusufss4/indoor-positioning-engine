#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "geo_internal.hpp"
#include "ipe/geo.hpp"

namespace ipe::geo {

using namespace detail;

// ---------------------------------------------------------------- venue JSON

namespace {

std::string coord(double v) { return ojson(round_mm(v)).dump(); }
std::string number(double v) { return ojson(v).dump(); }
std::string quoted(const std::string& s) { return ojson(s).dump(); }
std::string point(Vec2 p) { return "[" + coord(p.x) + ", " + coord(p.y) + "]"; }

}  // namespace

std::string venue_to_json(const Venue& v) {
  std::ostringstream o;
  const Bounds& b = v.bounds();
  o << "{\n";
  o << "  \"name\": " << quoted(v.name()) << ",\n";
  o << "  \"bounds\": [" << coord(b.min_x) << ", " << coord(b.min_y) << ", " << coord(b.max_x) << ", " << coord(b.max_y)
    << "],\n";
  o << "  \"wall_attenuation_db\": " << number(v.wall_attenuation_db()) << ",\n";
  o << "  \"floor_attenuation_db\": " << number(v.floor_attenuation_db()) << ",\n";
  if (const auto& g = v.georef()) {
    o << "  \"georef\": {\"origin_lat\": " << number(g->origin_lat) << ", \"origin_lon\": " << number(g->origin_lon)
      << ", \"rotation_deg\": " << number(g->rotation_deg) << "},\n";
  }
  if (!v.footprint().empty()) {
    o << "  \"footprint\": [";
    for (std::size_t k = 0; k < v.footprint().size(); ++k) o << (k ? ", " : "") << point(v.footprint()[k]);
    o << "],\n";
  }
  o << "  \"levels\": [\n";
  for (int i = 0; i < v.level_count(); ++i) {
    const Level& l = v.level(i);
    o << "    {\n      \"ordinal\": " << l.ordinal << ", \"name\": " << quoted(l.name)
      << ", \"elevation_m\": " << number(l.elevation_m) << ", \"height_m\": " << number(l.height_m) << ",\n";
    o << "      \"walls\": [";
    for (std::size_t k = 0; k < l.walls.size(); ++k) {
      const Segment& s = l.walls[k];
      o << (k ? ",\n        " : "\n        ") << "[" << coord(s.a.x) << ", " << coord(s.a.y) << ", " << coord(s.b.x)
        << ", " << coord(s.b.y) << "]";
    }
    o << (l.walls.empty() ? "]\n" : "\n      ]\n") << "    }" << (i + 1 < v.level_count() ? ",\n" : "\n");
  }
  o << "  ],\n";
  o << "  \"connectors\": [";
  for (std::size_t k = 0; k < v.connectors().size(); ++k) {
    const Connector& c = v.connectors()[k];
    o << (k ? ",\n    " : "\n    ") << "{\"id\": " << quoted(c.id)
      << ", \"kind\": " << quoted(std::string(to_string(c.kind))) << ", \"levels\": [";
    for (std::size_t i = 0; i < c.levels.size(); ++i) o << (i ? ", " : "") << v.level(c.levels[i]).ordinal;
    o << "], \"area\": [";
    for (std::size_t i = 0; i < c.area.size(); ++i) o << (i ? ", " : "") << point(c.area[i]);
    o << "]}";
  }
  o << (v.connectors().empty() ? "],\n" : "\n  ],\n");
  o << "  \"beacons\": [";
  for (std::size_t k = 0; k < v.beacons().size(); ++k) {
    const Beacon& bc = v.beacons()[k];
    o << (k ? ",\n    " : "\n    ") << "{\"id\": " << quoted(bc.id) << ", \"x\": " << coord(bc.pos.x)
      << ", \"y\": " << coord(bc.pos.y) << ", \"level\": " << v.level(bc.level).ordinal
      << ", \"tx_power\": " << number(bc.tx_power_dbm) << ", \"path_loss_exp\": " << number(bc.path_loss_exp) << "}";
  }
  o << (v.beacons().empty() ? "]\n" : "\n  ]\n");
  o << "}\n";
  return o.str();
}

// ------------------------------------------------------------ GeoJSON export

namespace {

ojson feature(ojson geometry, ojson properties) {
  return ojson{{"type", "Feature"}, {"properties", std::move(properties)}, {"geometry", std::move(geometry)}};
}

std::vector<Vec2> bounds_ring(const Bounds& b) {
  return {{b.min_x, b.min_y}, {b.max_x, b.min_y}, {b.max_x, b.max_y}, {b.min_x, b.max_y}};
}

const GeoRef& require_georef(const Venue& v) {
  if (!v.georef()) throw std::runtime_error("venue " + v.name() + " has no georef; add one to export it");
  return *v.georef();
}

}  // namespace

std::string export_geojson(const Venue& v, const ExportOptions& opts) {
  const GeoRef& g = require_georef(v);
  std::vector<ojson> fs;
  const std::vector<Vec2> outline = v.footprint().empty() ? bounds_ring(v.bounds()) : v.footprint();
  fs.push_back(feature(polygon(g, outline), {{"feature_type", "venue"},
                                             {"name", v.name()},
                                             {"wall_attenuation_db", v.wall_attenuation_db()},
                                             {"floor_attenuation_db", v.floor_attenuation_db()},
                                             {"origin_lat", g.origin_lat},
                                             {"origin_lon", g.origin_lon},
                                             {"rotation_deg", g.rotation_deg}}));
  for (const Level& l : v.levels()) {
    fs.push_back(feature(polygon(g, bounds_ring(v.bounds())), {{"feature_type", "level"},
                                                               {"level", l.ordinal},
                                                               {"ordinal", l.ordinal},
                                                               {"name", l.name},
                                                               {"elevation_m", l.elevation_m},
                                                               {"height_m", l.height_m}}));
  }
  for (const Level& l : v.levels()) {
    for (const Segment& s : l.walls) {
      fs.push_back(feature(
          polygon(g, wall_rect(s, opts.wall_thickness_m)),
          {{"feature_type", "wall"}, {"level", l.ordinal}, {"elevation_m", l.elevation_m}, {"height_m", l.height_m}}));
    }
  }
  for (const Connector& c : v.connectors()) {
    ojson ordinals = ojson::array();
    for (int i : c.levels) ordinals.push_back(v.level(i).ordinal);
    const Level& lo = v.level(c.levels.front());
    const Level& hi = v.level(c.levels.back());
    fs.push_back(feature(polygon(g, c.area), {{"feature_type", std::string(to_string(c.kind))},
                                              {"id", c.id},
                                              {"level", lo.ordinal},
                                              {"levels", ordinals},
                                              {"elevation_m", lo.elevation_m},
                                              {"height_m", hi.elevation_m + hi.height_m - lo.elevation_m}}));
  }
  for (const Beacon& b : v.beacons()) {
    const Level& l = v.level(b.level);
    fs.push_back(
        feature(ojson{{"type", "Point"}, {"coordinates", position(g, b.pos)}}, {{"feature_type", "beacon"},
                                                                                {"id", b.id},
                                                                                {"level", l.ordinal},
                                                                                {"elevation_m", l.elevation_m},
                                                                                {"tx_power", b.tx_power_dbm},
                                                                                {"path_loss_exp", b.path_loss_exp}}));
  }
  return feature_collection(fs);
}

// ------------------------------------------------------------- mapping table

Mapping parse_mapping(std::string_view json_text) {
  const json j = json::parse(json_text);
  Mapping m;
  const auto str = [&](const char* key, std::string& out) {
    if (j.contains(key)) out = j.at(key).get<std::string>();
  };
  str("type_property", m.type_property);
  str("level_property", m.level_property);
  str("levels_property", m.levels_property);
  str("ordinal_property", m.ordinal_property);
  str("name_property", m.name_property);
  str("id_property", m.id_property);
  str("elevation_property", m.elevation_property);
  str("height_property", m.height_property);
  str("tx_power_property", m.tx_power_property);
  str("path_loss_property", m.path_loss_property);
  if (j.contains("wall_max_thickness_m")) m.wall_max_thickness_m = j.at("wall_max_thickness_m").get<double>();
  if (j.contains("types")) {
    static const std::set<std::string> known{"wall",  "room",   "opening",  "level",    "beacon",
                                             "venue", "stairs", "elevator", "escalator"};
    for (const auto& [key, values] : j.at("types").items()) {
      if (!known.count(key)) throw std::runtime_error("mapping: unknown type '" + key + "'");
      auto list = values.is_array() ? values.get<std::vector<std::string>>()
                                    : std::vector<std::string>{values.get<std::string>()};
      if (key == "wall") m.wall = list;
      if (key == "room") m.room = list;
      if (key == "opening") m.opening = list;
      if (key == "level") m.level = list;
      if (key == "beacon") m.beacon = list;
      if (key == "venue") m.venue = list;
      if (key == "stairs") m.stairs = list;
      if (key == "elevator") m.elevator = list;
      if (key == "escalator") m.escalator = list;
    }
  }
  return m;
}

Mapping load_mapping_file(const std::string& path) { return parse_mapping(read_file(path)); }

// ------------------------------------------------------------ GeoJSON import

namespace {

enum class Kind { Wall, Room, Opening, Level, Beacon, Venue, Stairs, Elevator, Escalator, Other };

bool contains(const std::vector<std::string>& list, const std::string& v) {
  return std::find(list.begin(), list.end(), v) != list.end();
}

Kind classify(const Mapping& m, const std::string& type) {
  if (contains(m.wall, type)) return Kind::Wall;
  if (contains(m.room, type)) return Kind::Room;
  if (contains(m.opening, type)) return Kind::Opening;
  if (contains(m.level, type)) return Kind::Level;
  if (contains(m.beacon, type)) return Kind::Beacon;
  if (contains(m.venue, type)) return Kind::Venue;
  if (contains(m.stairs, type)) return Kind::Stairs;
  if (contains(m.elevator, type)) return Kind::Elevator;
  if (contains(m.escalator, type)) return Kind::Escalator;
  return Kind::Other;
}

std::optional<std::string> prop_string(const json& props, const std::string& key) {
  if (!props.is_object() || !props.contains(key)) return std::nullopt;
  const json& v = props.at(key);
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number()) return v.dump();
  if (v.is_object() && !v.empty()) {  // IMDF labels: {"en": "Ground"}
    if (v.contains("en") && v.at("en").is_string()) return v.at("en").get<std::string>();
    if (v.begin()->is_string()) return v.begin()->get<std::string>();
  }
  return std::nullopt;
}

std::optional<double> prop_number(const json& props, const std::string& key) {
  if (!props.is_object() || !props.contains(key)) return std::nullopt;
  const json& v = props.at(key);
  if (v.is_number()) return v.get<double>();
  if (v.is_string()) {
    try {
      std::size_t used = 0;
      const std::string s = v.get<std::string>();
      const double d = std::stod(s, &used);
      if (used == s.size()) return d;
    } catch (const std::exception&) {
    }
  }
  return std::nullopt;
}

std::optional<int> prop_int(const json& props, const std::string& key) {
  const auto d = prop_number(props, key);
  if (!d || std::abs(*d - std::round(*d)) > 1e-9) return std::nullopt;
  return static_cast<int>(std::lround(*d));
}

/// Geometry as lists of lon/lat points; `closed` for polygon rings.
struct Parts {
  bool closed = false;
  bool point = false;
  std::vector<std::vector<LatLon>> parts;
};

std::vector<LatLon> read_positions(const json& coords) {
  std::vector<LatLon> pts;
  for (const auto& c : coords) pts.push_back({c.at(1).get<double>(), c.at(0).get<double>()});
  return pts;
}

Parts read_geometry(const json& geom) {
  Parts p;
  if (geom.is_null()) return p;
  const std::string type = geom.at("type").get<std::string>();
  const json& c = geom.at("coordinates");
  if (type == "Point") {
    p.point = true;
    p.parts.push_back(read_positions(json::array({c})));
  } else if (type == "MultiPoint") {
    p.point = true;
    p.parts.push_back(read_positions(c));
  } else if (type == "LineString") {
    p.parts.push_back(read_positions(c));
  } else if (type == "MultiLineString") {
    for (const auto& l : c) p.parts.push_back(read_positions(l));
  } else if (type == "Polygon") {
    p.closed = true;
    for (const auto& r : c) p.parts.push_back(read_positions(r));
  } else if (type == "MultiPolygon") {
    p.closed = true;
    for (const auto& poly : c) {
      for (const auto& r : poly) p.parts.push_back(read_positions(r));
    }
  } else {
    throw std::runtime_error("geojson: unsupported geometry type " + type);
  }
  return p;
}

struct RawLevel {
  std::optional<std::string> name;
  std::optional<double> elevation;
  std::optional<double> height;
};

struct RawConnector {
  std::string id;
  ConnectorKind kind;
  Polygon area;
  std::set<int> ordinals;
};

struct RawBeacon {
  std::string id;
  Vec2 pos;
  int ordinal;
  double tx_power;
  double path_loss_exp;
};

/// Everything in the local frame of one georef.
struct Local {
  std::map<int, RawLevel> levels;
  std::map<int, std::vector<Segment>> walls;
  std::map<int, std::vector<std::vector<Vec2>>> openings;
  std::vector<RawConnector> connectors;
  std::vector<RawBeacon> beacons;
  Polygon footprint;
  Bounds level_extent{1e300, 1e300, -1e300, -1e300};
  Bounds all_extent{1e300, 1e300, -1e300, -1e300};
  std::map<std::string, int> unmapped;
  int without_level = 0;
};

void grow(Bounds& b, Vec2 p) {
  b.min_x = std::min(b.min_x, p.x);
  b.min_y = std::min(b.min_y, p.y);
  b.max_x = std::max(b.max_x, p.x);
  b.max_y = std::max(b.max_y, p.y);
}

Vec2 centroid(const Polygon& poly) {
  Vec2 c;
  for (const Vec2& p : poly) c += p;
  return poly.empty() ? c : c * (1.0 / static_cast<double>(poly.size()));
}

void add_edges(const std::vector<Vec2>& pts, bool closed, std::vector<Segment>& out) {
  for (std::size_t i = 0; i + 1 < pts.size(); ++i) out.push_back({pts[i], pts[i + 1]});
  if (closed && pts.size() > 2 && distance(pts.front(), pts.back()) > 1e-6) out.push_back({pts.back(), pts.front()});
}

Local to_local(const json& features, const Mapping& m, const GeoRef& g, double connector_merge_m) {
  Local out;
  for (const auto& f : features) {
    const json& props = f.contains("properties") ? f.at("properties") : json::object();
    const auto type = prop_string(props, m.type_property);
    const Kind kind = type ? classify(m, *type) : Kind::Other;
    if (kind == Kind::Other) {
      ++out.unmapped[type.value_or("(none)")];
      continue;
    }
    const Parts geom = read_geometry(f.contains("geometry") ? f.at("geometry") : json());
    if (kind == Kind::Venue) {
      // The outline becomes the footprint; holes and smaller parts are ignored.
      if (!geom.closed) continue;
      for (const auto& part : geom.parts) {
        Polygon ring;
        for (const LatLon& ll : part) {
          const Vec2 p = g.to_local(ll);
          ring.push_back({round_mm(p.x), round_mm(p.y)});
        }
        if (ring.size() > 3 && distance(ring.front(), ring.back()) < 1e-6) ring.pop_back();
        if (ring.size() >= 3 && polygon_area(ring) > polygon_area(out.footprint)) out.footprint = std::move(ring);
      }
      continue;
    }

    std::vector<std::vector<Vec2>> parts;
    for (const auto& part : geom.parts) {
      std::vector<Vec2> pts;
      for (const LatLon& ll : part) {
        pts.push_back(g.to_local(ll));
        grow(out.all_extent, pts.back());
      }
      parts.push_back(std::move(pts));
    }

    std::optional<int> ordinal = prop_int(props, kind == Kind::Level ? m.ordinal_property : m.level_property);
    if (!ordinal && kind == Kind::Level) ordinal = prop_int(props, m.level_property);
    if (!ordinal) {
      ++out.without_level;
      ordinal = 0;
    }

    switch (kind) {
      case Kind::Level: {
        auto& level = out.levels[*ordinal];
        if (!level.name) level.name = prop_string(props, m.name_property);
        if (!level.elevation) level.elevation = prop_number(props, m.elevation_property);
        if (!level.height) level.height = prop_number(props, m.height_property);
        for (const auto& part : parts) {
          for (const Vec2& p : part) grow(out.level_extent, p);
        }
        break;
      }
      case Kind::Wall: {
        out.levels.try_emplace(*ordinal);
        auto& walls = out.walls[*ordinal];
        for (const auto& part : parts) {
          if (geom.closed) {
            if (auto s = thin_rect_centerline(part, m.wall_max_thickness_m)) {
              walls.push_back(*s);
              continue;
            }
          }
          add_edges(part, geom.closed, walls);
        }
        break;
      }
      case Kind::Room: {
        out.levels.try_emplace(*ordinal);
        for (const auto& part : parts) add_edges(part, true, out.walls[*ordinal]);
        break;
      }
      case Kind::Opening: {
        for (const auto& part : parts) out.openings[*ordinal].push_back(part);
        break;
      }
      case Kind::Beacon: {
        out.levels.try_emplace(*ordinal);
        if (parts.empty() || parts.front().empty()) break;
        RawBeacon b;
        b.id = prop_string(props, m.id_property)
                   .value_or(f.contains("id") && f.at("id").is_string() ? f.at("id").get<std::string>()
                                                                        : "B" + std::to_string(out.beacons.size() + 1));
        b.pos = parts.front().front();
        b.ordinal = *ordinal;
        b.tx_power = prop_number(props, m.tx_power_property).value_or(-59.0);
        b.path_loss_exp = prop_number(props, m.path_loss_property).value_or(2.0);
        out.beacons.push_back(std::move(b));
        break;
      }
      case Kind::Stairs:
      case Kind::Elevator:
      case Kind::Escalator: {
        if (parts.empty() || parts.front().size() < 3) break;
        RawConnector c;
        c.kind = kind == Kind::Stairs     ? ConnectorKind::Stairs
                 : kind == Kind::Elevator ? ConnectorKind::Elevator
                                          : ConnectorKind::Escalator;
        c.id = prop_string(props, m.id_property)
                   .value_or(f.contains("id") && f.at("id").is_string() ? f.at("id").get<std::string>() : "");
        c.area = parts.front();
        if (c.area.size() > 3 && distance(c.area.front(), c.area.back()) < 1e-6) c.area.pop_back();
        if (props.contains(m.levels_property) && props.at(m.levels_property).is_array()) {
          for (const auto& o : props.at(m.levels_property)) c.ordinals.insert(o.get<int>());
        } else {
          c.ordinals.insert(*ordinal);
        }
        for (int o : c.ordinals) out.levels.try_emplace(o);
        // Platforms export one footprint per floor; stack them into one shaft.
        RawConnector* same = nullptr;
        for (auto& e : out.connectors) {
          if (e.kind != c.kind) continue;
          if ((!c.id.empty() && e.id == c.id) || distance(centroid(e.area), centroid(c.area)) < connector_merge_m) {
            same = &e;
            break;
          }
        }
        if (same) {
          same->ordinals.insert(c.ordinals.begin(), c.ordinals.end());
        } else {
          out.connectors.push_back(std::move(c));
        }
        break;
      }
      default:
        break;
    }
  }
  return out;
}

std::vector<RawBeacon> parse_beacon_sidecar(const std::string& text, const GeoRef& g, int default_ordinal) {
  const json j = json::parse(text);
  const json& list = j.is_object() ? j.at("beacons") : j;
  std::vector<RawBeacon> out;
  for (const auto& jb : list) {
    RawBeacon b;
    b.id = jb.at("id").get<std::string>();
    if (jb.contains("lat")) {
      b.pos = g.to_local({jb.at("lat").get<double>(), jb.at("lon").get<double>()});
    } else {
      b.pos = {jb.at("x").get<double>(), jb.at("y").get<double>()};
    }
    b.ordinal = jb.value("level", default_ordinal);
    b.tx_power = jb.value("tx_power", -59.0);
    b.path_loss_exp = jb.value("path_loss_exp", 2.0);
    out.push_back(std::move(b));
  }
  return out;
}

}  // namespace

ImportResult import_geojson(std::string_view geojson, const Mapping& m, const ImportOptions& opts) {
  const json j = json::parse(geojson);
  if (j.value("type", "") != "FeatureCollection") throw std::runtime_error("geojson: expected a FeatureCollection");
  const json& features = j.at("features");

  ImportResult result;
  std::string name = opts.name;
  std::optional<GeoRef> georef = opts.georef;
  std::optional<double> wall_att = opts.wall_attenuation_db;
  std::optional<double> floor_att = opts.floor_attenuation_db;
  double lat_sum = 0.0, lon_sum = 0.0;
  std::size_t n_pos = 0;
  for (const auto& f : features) {
    const json& props = f.contains("properties") ? f.at("properties") : json::object();
    const auto type = prop_string(props, m.type_property);
    if (type && classify(m, *type) == Kind::Venue) {
      if (name.empty()) name = prop_string(props, m.name_property).value_or("");
      if (!wall_att) wall_att = prop_number(props, "wall_attenuation_db");
      if (!floor_att) floor_att = prop_number(props, "floor_attenuation_db");
      const auto lat = prop_number(props, "origin_lat");
      const auto lon = prop_number(props, "origin_lon");
      if (!georef && lat && lon) georef = GeoRef{*lat, *lon, prop_number(props, "rotation_deg").value_or(0.0)};
    }
    if (f.contains("geometry")) {
      for (const auto& part : read_geometry(f.at("geometry")).parts) {
        for (const LatLon& ll : part) {
          lat_sum += ll.lat;
          lon_sum += ll.lon;
          ++n_pos;
        }
      }
    }
  }
  if (n_pos == 0) throw std::runtime_error("geojson: no geometry");

  Local local;
  if (georef) {
    local = to_local(features, m, *georef, opts.connector_merge_m);
  } else {
    // No anchor in the data: anchor the frame at the south-west corner of the
    // map so local coordinates are small and positive.
    const GeoRef provisional{lat_sum / static_cast<double>(n_pos), lon_sum / static_cast<double>(n_pos), 0.0};
    const Local tmp = to_local(features, m, provisional, opts.connector_merge_m);
    const Bounds& e = tmp.level_extent.min_x <= tmp.level_extent.max_x ? tmp.level_extent : tmp.all_extent;
    const LatLon sw = provisional.to_wgs84({e.min_x, e.min_y});
    georef = GeoRef{round_deg(sw.lat), round_deg(sw.lon), 0.0};
    local = to_local(features, m, *georef, opts.connector_merge_m);
    result.warnings.push_back("no georef in the data; anchored the local frame at the south-west corner");
  }

  for (const auto& [type, count] : local.unmapped) {
    result.warnings.push_back("ignored " + std::to_string(count) + " feature(s) of unmapped type '" + type + "'");
  }
  if (local.without_level > 0) {
    result.warnings.push_back(std::to_string(local.without_level) +
                              " feature(s) had no level property and were put on ordinal 0");
  }
  if (local.levels.empty()) local.levels.try_emplace(0);

  const Bounds& e = local.level_extent.min_x <= local.level_extent.max_x ? local.level_extent : local.all_extent;
  const Bounds bounds{round_mm(e.min_x), round_mm(e.min_y), round_mm(e.max_x), round_mm(e.max_y)};

  std::vector<Level> levels;
  std::map<int, int> index;
  for (const auto& [ordinal, raw] : local.levels) {
    Level l;
    l.ordinal = ordinal;
    l.name = raw.name.value_or("L" + std::to_string(ordinal));
    l.height_m = raw.height.value_or(opts.level_height_m);
    l.elevation_m = raw.elevation.value_or(ordinal * l.height_m);
    std::vector<Segment> walls = merge_collinear(local.walls[ordinal]);
    walls = cut_openings(walls, local.openings[ordinal], 0.5 * m.wall_max_thickness_m);
    for (auto& s : walls) {
      s = {{round_mm(s.a.x), round_mm(s.a.y)}, {round_mm(s.b.x), round_mm(s.b.y)}};
      if (distance(s.a, s.b) >= opts.min_wall_m) l.walls.push_back(s);
    }
    index[ordinal] = static_cast<int>(levels.size());
    levels.push_back(std::move(l));
  }

  std::vector<Connector> connectors;
  int unnamed = 0;
  for (const auto& rc : local.connectors) {
    const std::string id = rc.id.empty() ? std::string(to_string(rc.kind)) + "_" + std::to_string(++unnamed) : rc.id;
    if (rc.ordinals.size() < 2) {
      result.warnings.push_back("connector " + id + " serves a single level; dropped");
      continue;
    }
    Connector c;
    c.id = id;
    c.kind = rc.kind;
    for (const Vec2& p : rc.area) c.area.push_back({round_mm(p.x), round_mm(p.y)});
    for (int o : rc.ordinals) c.levels.push_back(index.at(o));
    connectors.push_back(std::move(c));
  }

  std::vector<RawBeacon> raw_beacons = local.beacons;
  if (!opts.beacons_json.empty())
    raw_beacons = parse_beacon_sidecar(opts.beacons_json, *georef, levels.front().ordinal);
  if (raw_beacons.empty() && opts.beacon_grid_m > 0.0) {
    const double s = opts.beacon_grid_m;
    for (const Level& l : levels) {
      int k = 0;
      for (double y = bounds.min_y + 0.5 * s; y < bounds.max_y; y += s) {
        for (double x = bounds.min_x + 0.5 * s; x < bounds.max_x; x += s) {
          raw_beacons.push_back(
              {"G" + std::to_string(l.ordinal) + "-" + std::to_string(++k), {x, y}, l.ordinal, -59.0, 2.0});
        }
      }
    }
    result.warnings.push_back("placed " + std::to_string(raw_beacons.size()) + " beacons on a " + ojson(s).dump() +
                              " m grid");
  }
  if (raw_beacons.empty()) result.warnings.push_back("no beacons: positioning will rely on dead reckoning alone");

  std::vector<Beacon> beacons;
  for (const auto& rb : raw_beacons) {
    const auto it = index.find(rb.ordinal);
    if (it == index.end())
      throw std::runtime_error("beacon " + rb.id + " is on unknown level " + std::to_string(rb.ordinal));
    beacons.push_back({rb.id, {round_mm(rb.pos.x), round_mm(rb.pos.y)}, it->second, rb.tx_power, rb.path_loss_exp});
  }

  result.venue =
      Venue(name.empty() ? "imported" : name, bounds, std::move(levels), std::move(beacons), std::move(connectors),
            wall_att.value_or(3.5), floor_att.value_or(15.0), georef, std::move(local.footprint));
  return result;
}

}  // namespace ipe::geo
