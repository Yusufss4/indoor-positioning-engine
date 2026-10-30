#include "ipe/venue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

std::string_view to_string(ConnectorKind kind) {
  switch (kind) {
    case ConnectorKind::Stairs:
      return "stairs";
    case ConnectorKind::Elevator:
      return "elevator";
    case ConnectorKind::Escalator:
      return "escalator";
  }
  return "unknown";
}

ConnectorKind parse_connector_kind(std::string_view name) {
  if (name == "stairs") return ConnectorKind::Stairs;
  if (name == "elevator") return ConnectorKind::Elevator;
  if (name == "escalator") return ConnectorKind::Escalator;
  throw std::invalid_argument("unknown connector kind '" + std::string(name) +
                              "' (expected stairs, elevator or escalator)");
}

bool Connector::serves(int level) const { return std::find(levels.begin(), levels.end(), level) != levels.end(); }

Venue::Venue(std::string name, Bounds bounds, std::vector<Segment> walls, std::vector<Beacon> beacons,
             double wall_attenuation_db)
    : name_(std::move(name)), bounds_(bounds), beacons_(std::move(beacons)), wall_attenuation_db_(wall_attenuation_db) {
  Level ground;
  ground.walls = std::move(walls);
  levels_.push_back(std::move(ground));
  validate();
  build_grids();
}

Venue::Venue(std::string name, Bounds bounds, std::vector<Level> levels, std::vector<Beacon> beacons,
             std::vector<Connector> connectors, double wall_attenuation_db, double floor_attenuation_db,
             std::optional<GeoRef> georef, Polygon footprint, std::vector<Room> rooms)
    : name_(std::move(name)),
      bounds_(bounds),
      footprint_(std::move(footprint)),
      levels_(std::move(levels)),
      beacons_(std::move(beacons)),
      connectors_(std::move(connectors)),
      rooms_(std::move(rooms)),
      wall_attenuation_db_(wall_attenuation_db),
      floor_attenuation_db_(floor_attenuation_db),
      georef_(georef) {
  for (auto& c : connectors_) std::sort(c.levels.begin(), c.levels.end());
  if (footprint_.size() < 3 || polygon_area(footprint_) >= 0.99 * bounds_.width() * bounds_.height()) {
    footprint_.clear();
  }
  for (auto& r : rooms_) {
    if (r.area.size() > 3 && distance(r.area.front(), r.area.back()) < 1e-9) r.area.pop_back();
    if (r.area.empty()) continue;
    r.box = {r.area.front().x, r.area.front().y, r.area.front().x, r.area.front().y};
    for (const Vec2& p : r.area) {
      r.box.min_x = std::min(r.box.min_x, p.x);
      r.box.min_y = std::min(r.box.min_y, p.y);
      r.box.max_x = std::max(r.box.max_x, p.x);
      r.box.max_y = std::max(r.box.max_y, p.y);
    }
  }
  validate();
  build_grids();
  build_room_grids();
}

void Venue::validate() const {
  if (levels_.empty()) throw std::runtime_error("venue: at least one level is required");
  for (std::size_t i = 1; i < levels_.size(); ++i) {
    if (levels_[i].ordinal <= levels_[i - 1].ordinal) {
      throw std::runtime_error("venue: levels must have distinct ordinals sorted ascending");
    }
  }
  const auto valid_level = [&](int l) { return l >= 0 && l < level_count(); };
  for (const auto& b : beacons_) {
    if (!valid_level(b.level)) throw std::runtime_error("venue: beacon " + b.id + " on unknown level");
  }
  for (const auto& c : connectors_) {
    if (c.area.size() < 3) throw std::runtime_error("venue: connector " + c.id + " needs a polygon area");
    if (c.levels.size() < 2) throw std::runtime_error("venue: connector " + c.id + " must serve two or more levels");
    if (!std::all_of(c.levels.begin(), c.levels.end(), valid_level)) {
      throw std::runtime_error("venue: connector " + c.id + " serves an unknown level");
    }
  }
  for (std::size_t i = 0; i < rooms_.size(); ++i) {
    const Room& r = rooms_[i];
    if (r.id.empty()) throw std::runtime_error("venue: room without id");
    if (!valid_level(r.level)) throw std::runtime_error("venue: room " + r.id + " on unknown level");
    if (r.area.size() < 3 || polygon_area(r.area) < 1e-6) {
      throw std::runtime_error("venue: room " + r.id + " needs a polygon area");
    }
    for (std::size_t k = 0; k < i; ++k) {
      if (rooms_[k].id == r.id) throw std::runtime_error("venue: duplicate room id " + r.id);
    }
  }
}

const std::vector<Segment>& Venue::walls(int level) const {
  static const std::vector<Segment> none;
  return level >= 0 && level < level_count() ? levels_[static_cast<std::size_t>(level)].walls : none;
}

std::optional<int> Venue::level_index(int ordinal) const {
  for (int i = 0; i < level_count(); ++i) {
    if (levels_[static_cast<std::size_t>(i)].ordinal == ordinal) return i;
  }
  return std::nullopt;
}

int Venue::nearest_level(double z) const {
  int best = 0;
  double best_d = std::numeric_limits<double>::infinity();
  for (int i = 0; i < level_count(); ++i) {
    const double d = std::abs(levels_[static_cast<std::size_t>(i)].elevation_m - z);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

const Connector* Venue::connector_at(Vec2 p, int level) const {
  for (const auto& c : connectors_) {
    if (c.serves(level) && c.contains(p)) return &c;
  }
  return nullptr;
}

std::optional<std::size_t> Venue::room_index(std::string_view id) const {
  for (std::size_t i = 0; i < rooms_.size(); ++i) {
    if (rooms_[i].id == id) return i;
  }
  return std::nullopt;
}

std::span<const std::size_t> Venue::room_candidates(Vec2 p, int level) const {
  if (level < 0 || level >= static_cast<int>(room_grids_.size())) return {};
  const RoomGrid& grid = room_grids_[static_cast<std::size_t>(level)];
  if (grid.cells.empty() || !bounds_.contains(p)) return {};
  const int x = std::min(static_cast<int>((p.x - bounds_.min_x) / grid.cell), grid.nx - 1);
  const int y = std::min(static_cast<int>((p.y - bounds_.min_y) / grid.cell), grid.ny - 1);
  return grid.cells[static_cast<std::size_t>(y * grid.nx + x)];
}

void Venue::rooms_at(Vec2 p, int level, std::vector<std::size_t>& out) const {
  out.clear();
  for (std::size_t r : room_candidates(p, level)) {
    if (rooms_[r].contains(p)) out.push_back(r);
  }
}

void Venue::build_room_grids() {
  room_grids_.assign(levels_.size(), RoomGrid{});
  if (rooms_.empty()) return;
  for (auto& grid : room_grids_) {
    grid.nx = std::max(1, static_cast<int>(std::ceil(bounds_.width() / grid.cell)));
    grid.ny = std::max(1, static_cast<int>(std::ceil(bounds_.height() / grid.cell)));
    grid.cells.assign(static_cast<std::size_t>(grid.nx * grid.ny), {});
  }
  for (std::size_t r = 0; r < rooms_.size(); ++r) {
    const Room& room = rooms_[r];
    RoomGrid& grid = room_grids_[static_cast<std::size_t>(room.level)];
    auto cell_of = [&](double v, double lo, int n) {
      return std::clamp(static_cast<int>(std::floor((v - lo) / grid.cell)), 0, n - 1);
    };
    const int x0 = cell_of(room.box.min_x, bounds_.min_x, grid.nx);
    const int x1 = cell_of(room.box.max_x, bounds_.min_x, grid.nx);
    const int y0 = cell_of(room.box.min_y, bounds_.min_y, grid.ny);
    const int y1 = cell_of(room.box.max_y, bounds_.min_y, grid.ny);
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) grid.cells[static_cast<std::size_t>(y * grid.nx + x)].push_back(r);
    }
  }
}

std::optional<std::size_t> Venue::beacon_index(std::string_view id) const {
  for (std::size_t i = 0; i < beacons_.size(); ++i) {
    if (beacons_[i].id == id) return i;
  }
  return std::nullopt;
}

void Venue::build_grids() {
  grids_.assign(levels_.size(), Grid{});
  for (std::size_t l = 0; l < levels_.size(); ++l) {
    Grid& grid = grids_[l];
    const auto& walls = levels_[l].walls;
    grid.nx = std::max(1, static_cast<int>(std::ceil(bounds_.width() / grid.cell)));
    grid.ny = std::max(1, static_cast<int>(std::ceil(bounds_.height() / grid.cell)));
    grid.cells.assign(static_cast<std::size_t>(grid.nx * grid.ny), {});

    auto cell_of = [&](double v, double lo, int n) {
      return std::clamp(static_cast<int>(std::floor((v - lo) / grid.cell)), 0, n - 1);
    };
    for (std::size_t w = 0; w < walls.size(); ++w) {
      const auto& s = walls[w];
      const int x0 = cell_of(std::min(s.a.x, s.b.x), bounds_.min_x, grid.nx);
      const int x1 = cell_of(std::max(s.a.x, s.b.x), bounds_.min_x, grid.nx);
      const int y0 = cell_of(std::min(s.a.y, s.b.y), bounds_.min_y, grid.ny);
      const int y1 = cell_of(std::max(s.a.y, s.b.y), bounds_.min_y, grid.ny);
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) grid.cells[static_cast<std::size_t>(y * grid.nx + x)].push_back(w);
      }
    }
  }
}

template <typename Fn>
void Venue::for_each_candidate_wall(Vec2 a, Vec2 b, int level, Fn&& fn) const {
  if (level < 0 || level >= static_cast<int>(grids_.size())) return;
  const Grid& grid = grids_[static_cast<std::size_t>(level)];
  const auto& walls = levels_[static_cast<std::size_t>(level)].walls;
  auto cell_of = [&](double v, double lo, int n) {
    return std::clamp(static_cast<int>(std::floor((v - lo) / grid.cell)), 0, n - 1);
  };
  const int x0 = cell_of(std::min(a.x, b.x), bounds_.min_x, grid.nx);
  const int x1 = cell_of(std::max(a.x, b.x), bounds_.min_x, grid.nx);
  const int y0 = cell_of(std::min(a.y, b.y), bounds_.min_y, grid.ny);
  const int y1 = cell_of(std::max(a.y, b.y), bounds_.min_y, grid.ny);
  const auto n_cells = static_cast<std::size_t>((x1 - x0 + 1) * (y1 - y0 + 1));

  // Long radio paths cover most of the grid; a linear scan is cheaper there.
  if (n_cells * 2 > walls.size()) {
    for (std::size_t w = 0; w < walls.size(); ++w) {
      if (!fn(walls[w])) return;
    }
    return;
  }

  // Walls spanning several cells must only be visited once per query.
  thread_local std::vector<std::uint32_t> stamps;
  thread_local std::uint32_t stamp = 0;
  if (stamps.size() < walls.size()) stamps.resize(walls.size(), 0);
  if (++stamp == 0) {
    std::fill(stamps.begin(), stamps.end(), 0);
    stamp = 1;
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      for (std::size_t w : grid.cells[static_cast<std::size_t>(y * grid.nx + x)]) {
        if (stamps[w] == stamp) continue;
        stamps[w] = stamp;
        if (!fn(walls[w])) return;
      }
    }
  }
}

bool Venue::crosses_wall(Vec2 a, Vec2 b, int level) const {
  bool hit = false;
  for_each_candidate_wall(a, b, level, [&](const Segment& w) {
    if (segments_intersect(a, b, w.a, w.b)) {
      hit = true;
      return false;
    }
    return true;
  });
  return hit;
}

int Venue::count_wall_crossings(Vec2 a, Vec2 b, int level) const {
  int n = 0;
  for_each_candidate_wall(a, b, level, [&](const Segment& w) {
    if (segments_intersect(a, b, w.a, w.b)) ++n;
    return true;
  });
  return n;
}

namespace {

using nlohmann::json;

std::vector<Segment> parse_walls(const json& arr) {
  std::vector<Segment> walls;
  for (const auto& w : arr) {
    if (!w.is_array() || w.size() != 4) throw std::runtime_error("venue: wall must be [x1, y1, x2, y2]");
    walls.push_back({{w[0].get<double>(), w[1].get<double>()}, {w[2].get<double>(), w[3].get<double>()}});
  }
  return walls;
}

}  // namespace

Venue parse_venue(std::string_view json_text) {
  const auto j = json::parse(json_text);

  const auto& b = j.at("bounds");
  if (!b.is_array() || b.size() != 4) throw std::runtime_error("venue: bounds must be [min_x, min_y, max_x, max_y]");
  Bounds bounds{b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
  if (bounds.width() <= 0.0 || bounds.height() <= 0.0) throw std::runtime_error("venue: empty bounds");

  std::vector<Level> levels;
  if (j.contains("levels")) {
    if (j.contains("walls")) throw std::runtime_error("venue: use either top-level walls or levels, not both");
    for (const auto& jl : j.at("levels")) {
      Level level;
      level.ordinal = jl.at("ordinal").get<int>();
      level.name = jl.value("name", "L" + std::to_string(level.ordinal));
      level.height_m = jl.value("height_m", 4.0);
      level.elevation_m = jl.value("elevation_m", level.ordinal * level.height_m);
      level.walls = parse_walls(jl.value("walls", json::array()));
      levels.push_back(std::move(level));
    }
    std::sort(levels.begin(), levels.end(), [](const Level& a, const Level& c) { return a.ordinal < c.ordinal; });
  } else {
    Level ground;
    ground.walls = parse_walls(j.value("walls", json::array()));
    levels.push_back(std::move(ground));
  }

  const auto index_of = [&](int ordinal, const std::string& what) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
      if (levels[i].ordinal == ordinal) return static_cast<int>(i);
    }
    throw std::runtime_error("venue: " + what + " references unknown level " + std::to_string(ordinal));
  };

  std::vector<Beacon> beacons;
  for (const auto& jb : j.at("beacons")) {
    Beacon beacon;
    beacon.id = jb.at("id").get<std::string>();
    beacon.pos = {jb.at("x").get<double>(), jb.at("y").get<double>()};
    beacon.level = index_of(jb.value("level", levels.front().ordinal), "beacon " + beacon.id);
    beacon.tx_power_dbm = jb.value("tx_power", -59.0);
    beacon.path_loss_exp = jb.value("path_loss_exp", 2.0);
    beacons.push_back(std::move(beacon));
  }

  std::vector<Connector> connectors;
  for (const auto& jc : j.value("connectors", json::array())) {
    Connector c;
    c.id = jc.at("id").get<std::string>();
    c.kind = parse_connector_kind(jc.value("kind", std::string("stairs")));
    for (const auto& p : jc.at("area")) {
      if (!p.is_array() || p.size() != 2) throw std::runtime_error("venue: connector area points must be [x, y]");
      c.area.push_back({p[0].get<double>(), p[1].get<double>()});
    }
    for (const auto& o : jc.at("levels")) c.levels.push_back(index_of(o.get<int>(), "connector " + c.id));
    connectors.push_back(std::move(c));
  }

  std::vector<Room> rooms;
  for (const auto& jr : j.value("rooms", json::array())) {
    Room r;
    r.id = jr.at("id").get<std::string>();
    r.name = jr.value("name", r.id);
    r.level = index_of(jr.value("level", levels.front().ordinal), "room " + r.id);
    for (const auto& p : jr.at("area")) {
      if (!p.is_array() || p.size() != 2) throw std::runtime_error("venue: room area points must be [x, y]");
      r.area.push_back({p[0].get<double>(), p[1].get<double>()});
    }
    rooms.push_back(std::move(r));
  }

  std::optional<GeoRef> georef;
  if (j.contains("georef")) {
    const auto& g = j.at("georef");
    georef = GeoRef{g.at("origin_lat").get<double>(), g.at("origin_lon").get<double>(), g.value("rotation_deg", 0.0)};
  }

  Polygon footprint;
  for (const auto& p : j.value("footprint", json::array())) {
    if (!p.is_array() || p.size() != 2) throw std::runtime_error("venue: footprint points must be [x, y]");
    footprint.push_back({p[0].get<double>(), p[1].get<double>()});
  }
  if (!footprint.empty() && footprint.size() < 3) throw std::runtime_error("venue: footprint needs 3 or more points");

  return Venue(j.value("name", std::string("unnamed")), bounds, std::move(levels), std::move(beacons),
               std::move(connectors), j.value("wall_attenuation_db", 3.0), j.value("floor_attenuation_db", 15.0),
               georef, std::move(footprint), std::move(rooms));
}

Venue load_venue_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open venue file: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_venue(ss.str());
}

}  // namespace ipe
