#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/geometry.hpp"

namespace ipe {

struct Beacon {
  std::string id;
  Vec2 pos;
  double tx_power_dbm = -59.0;  ///< Expected RSSI at 1 m.
  double path_loss_exp = 2.0;   ///< Log-distance path-loss exponent.
};

/// Static description of a venue floor: walls (impassable for pedestrians,
/// attenuating for radio) and the surveyed beacon positions.
class Venue {
 public:
  Venue() = default;
  Venue(std::string name, Bounds bounds, std::vector<Segment> walls, std::vector<Beacon> beacons,
        double wall_attenuation_db);

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const Bounds& bounds() const { return bounds_; }
  [[nodiscard]] const std::vector<Segment>& walls() const { return walls_; }
  [[nodiscard]] const std::vector<Beacon>& beacons() const { return beacons_; }
  [[nodiscard]] double wall_attenuation_db() const { return wall_attenuation_db_; }

  [[nodiscard]] std::optional<std::size_t> beacon_index(std::string_view id) const;

  /// True if moving in a straight line from a to b crosses any wall.
  [[nodiscard]] bool crosses_wall(Vec2 a, Vec2 b) const;

  /// Number of walls intersected by the straight line a-b (radio path).
  [[nodiscard]] int count_wall_crossings(Vec2 a, Vec2 b) const;

 private:
  /// Walls bucketed into a uniform grid so path queries only test nearby walls.
  struct Grid {
    double cell = 2.0;
    int nx = 0;
    int ny = 0;
    std::vector<std::vector<std::size_t>> cells;
  };

  void build_grid();
  template <typename Fn>
  void for_each_candidate_wall(Vec2 a, Vec2 b, Fn&& fn) const;

  std::string name_;
  Bounds bounds_{};
  std::vector<Segment> walls_;
  std::vector<Beacon> beacons_;
  double wall_attenuation_db_ = 3.0;
  Grid grid_;
};

/// Parses the venue JSON format documented in docs/formats.md.
Venue parse_venue(std::string_view json_text);
Venue load_venue_file(const std::string& path);

}  // namespace ipe
