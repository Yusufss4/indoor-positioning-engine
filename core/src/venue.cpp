#include "ipe/venue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

Venue::Venue(std::string name, Bounds bounds, std::vector<Segment> walls,
             std::vector<Beacon> beacons, double wall_attenuation_db)
    : name_(std::move(name)),
      bounds_(bounds),
      walls_(std::move(walls)),
      beacons_(std::move(beacons)),
      wall_attenuation_db_(wall_attenuation_db) {
  build_grid();
}

std::optional<std::size_t> Venue::beacon_index(std::string_view id) const {
  for (std::size_t i = 0; i < beacons_.size(); ++i) {
    if (beacons_[i].id == id) return i;
  }
  return std::nullopt;
}

void Venue::build_grid() {
  grid_.nx = std::max(1, static_cast<int>(std::ceil(bounds_.width() / grid_.cell)));
  grid_.ny = std::max(1, static_cast<int>(std::ceil(bounds_.height() / grid_.cell)));
  grid_.cells.assign(static_cast<std::size_t>(grid_.nx * grid_.ny), {});

  auto cell_of = [&](double v, double lo, int n) {
    return std::clamp(static_cast<int>(std::floor((v - lo) / grid_.cell)), 0, n - 1);
  };
  for (std::size_t w = 0; w < walls_.size(); ++w) {
    const auto& s = walls_[w];
    const int x0 = cell_of(std::min(s.a.x, s.b.x), bounds_.min_x, grid_.nx);
    const int x1 = cell_of(std::max(s.a.x, s.b.x), bounds_.min_x, grid_.nx);
    const int y0 = cell_of(std::min(s.a.y, s.b.y), bounds_.min_y, grid_.ny);
    const int y1 = cell_of(std::max(s.a.y, s.b.y), bounds_.min_y, grid_.ny);
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) grid_.cells[static_cast<std::size_t>(y * grid_.nx + x)].push_back(w);
    }
  }
}

template <typename Fn>
void Venue::for_each_candidate_wall(Vec2 a, Vec2 b, Fn&& fn) const {
  auto cell_of = [&](double v, double lo, int n) {
    return std::clamp(static_cast<int>(std::floor((v - lo) / grid_.cell)), 0, n - 1);
  };
  const int x0 = cell_of(std::min(a.x, b.x), bounds_.min_x, grid_.nx);
  const int x1 = cell_of(std::max(a.x, b.x), bounds_.min_x, grid_.nx);
  const int y0 = cell_of(std::min(a.y, b.y), bounds_.min_y, grid_.ny);
  const int y1 = cell_of(std::max(a.y, b.y), bounds_.min_y, grid_.ny);
  const auto n_cells = static_cast<std::size_t>((x1 - x0 + 1) * (y1 - y0 + 1));

  // Long radio paths cover most of the grid; a linear scan is cheaper there.
  if (n_cells * 2 > walls_.size()) {
    for (std::size_t w = 0; w < walls_.size(); ++w) {
      if (!fn(w)) return;
    }
    return;
  }

  // Walls spanning several cells must only be visited once per query.
  thread_local std::vector<std::uint32_t> stamps;
  thread_local std::uint32_t stamp = 0;
  if (stamps.size() < walls_.size()) stamps.resize(walls_.size(), 0);
  if (++stamp == 0) {
    std::fill(stamps.begin(), stamps.end(), 0);
    stamp = 1;
  }
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      for (std::size_t w : grid_.cells[static_cast<std::size_t>(y * grid_.nx + x)]) {
        if (stamps[w] == stamp) continue;
        stamps[w] = stamp;
        if (!fn(w)) return;
      }
    }
  }
}

bool Venue::crosses_wall(Vec2 a, Vec2 b) const {
  bool hit = false;
  for_each_candidate_wall(a, b, [&](std::size_t w) {
    if (segments_intersect(a, b, walls_[w].a, walls_[w].b)) {
      hit = true;
      return false;
    }
    return true;
  });
  return hit;
}

int Venue::count_wall_crossings(Vec2 a, Vec2 b) const {
  int n = 0;
  for_each_candidate_wall(a, b, [&](std::size_t w) {
    if (segments_intersect(a, b, walls_[w].a, walls_[w].b)) ++n;
    return true;
  });
  return n;
}

Venue parse_venue(std::string_view json_text) {
  const auto j = nlohmann::json::parse(json_text);

  const auto& b = j.at("bounds");
  if (!b.is_array() || b.size() != 4) throw std::runtime_error("venue: bounds must be [min_x, min_y, max_x, max_y]");
  Bounds bounds{b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
  if (bounds.width() <= 0.0 || bounds.height() <= 0.0) throw std::runtime_error("venue: empty bounds");

  std::vector<Segment> walls;
  for (const auto& w : j.value("walls", nlohmann::json::array())) {
    if (!w.is_array() || w.size() != 4) throw std::runtime_error("venue: wall must be [x1, y1, x2, y2]");
    walls.push_back({{w[0].get<double>(), w[1].get<double>()}, {w[2].get<double>(), w[3].get<double>()}});
  }

  std::vector<Beacon> beacons;
  for (const auto& jb : j.at("beacons")) {
    Beacon beacon;
    beacon.id = jb.at("id").get<std::string>();
    beacon.pos = {jb.at("x").get<double>(), jb.at("y").get<double>()};
    beacon.tx_power_dbm = jb.value("tx_power", -59.0);
    beacon.path_loss_exp = jb.value("path_loss_exp", 2.0);
    beacons.push_back(std::move(beacon));
  }

  return Venue(j.value("name", std::string("unnamed")), bounds, std::move(walls), std::move(beacons),
               j.value("wall_attenuation_db", 3.0));
}

Venue load_venue_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open venue file: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_venue(ss.str());
}

}  // namespace ipe
