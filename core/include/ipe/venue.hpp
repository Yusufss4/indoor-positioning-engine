#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/georef.hpp"

namespace ipe {

struct Beacon {
  std::string id;
  Vec2 pos;
  int level = 0;                ///< Level index (see Venue::levels()).
  double tx_power_dbm = -59.0;  ///< Expected RSSI at 1 m.
  double path_loss_exp = 2.0;   ///< Log-distance path-loss exponent.
};

/// One floor of the venue. Levels are stored sorted by `ordinal`; everywhere
/// in the engine a level is referred to by its index in that list.
struct Level {
  std::string name;
  int ordinal = 0;           ///< Building floor number as in IMDF (0 = ground, -1 = basement).
  double elevation_m = 0.0;  ///< Floor height above the venue datum.
  double height_m = 4.0;     ///< Floor-to-floor height, used for rendering.
  std::vector<Segment> walls;
};

enum class ConnectorKind { Stairs, Elevator, Escalator };

std::string_view to_string(ConnectorKind kind);
/// Accepts "stairs", "elevator" and "escalator". Throws std::invalid_argument otherwise.
ConnectorKind parse_connector_kind(std::string_view name);

/// A vertical connection (stairwell, lift, escalator). Its footprint is the
/// only place where a pedestrian can change level.
struct Connector {
  std::string id;
  ConnectorKind kind = ConnectorKind::Stairs;
  Polygon area;
  std::vector<int> levels;  ///< Level indices served, ascending.

  [[nodiscard]] bool serves(int level) const;
  [[nodiscard]] bool contains(Vec2 p) const { return point_in_polygon(p, area); }
};

/// A named area on one level (room, shop, corridor) that enter/exit events
/// refer to. Rooms are not obstacles; they may overlap, e.g. a wing and the
/// rooms inside it.
struct Room {
  std::string id;
  std::string name;
  int level = 0;  ///< Level index.
  Polygon area;
  Bounds box{};  ///< Bounding box of `area`; filled in by Venue.

  [[nodiscard]] bool contains(Vec2 p) const { return box.contains(p) && point_in_polygon(p, area); }
};

/// Static description of a venue: one or more floors with walls (impassable
/// for pedestrians, attenuating for radio), the surveyed beacon positions,
/// the stairs/lifts connecting the floors and the named rooms.
class Venue {
 public:
  Venue() = default;
  /// Single-level venue (level 0 at elevation 0).
  Venue(std::string name, Bounds bounds, std::vector<Segment> walls, std::vector<Beacon> beacons,
        double wall_attenuation_db);
  /// Multi-level venue. `levels` are sorted by ordinal; beacon and connector
  /// level indices refer to the sorted order. `footprint` is the outline of
  /// the walkable area on every level (e.g. the building outline); a footprint
  /// covering at least 99% of `bounds` adds nothing and is dropped.
  Venue(std::string name, Bounds bounds, std::vector<Level> levels, std::vector<Beacon> beacons,
        std::vector<Connector> connectors, double wall_attenuation_db, double floor_attenuation_db,
        std::optional<GeoRef> georef = std::nullopt, Polygon footprint = {}, std::vector<Room> rooms = {});

  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const Bounds& bounds() const { return bounds_; }
  /// Empty when the whole bounds rectangle is walkable.
  [[nodiscard]] const Polygon& footprint() const { return footprint_; }
  /// True inside the bounds and, if there is one, the footprint.
  [[nodiscard]] bool inside(Vec2 p) const {
    return bounds_.contains(p) && (footprint_.empty() || point_in_polygon(p, footprint_));
  }
  /// Area a position can be in: the footprint's, else the bounds'.
  [[nodiscard]] double walkable_area() const {
    return footprint_.empty() ? bounds_.width() * bounds_.height() : polygon_area(footprint_);
  }
  [[nodiscard]] const std::vector<Segment>& walls(int level = 0) const;
  [[nodiscard]] const std::vector<Beacon>& beacons() const { return beacons_; }
  [[nodiscard]] double wall_attenuation_db() const { return wall_attenuation_db_; }
  /// Loss per concrete slab between transmitter and receiver levels.
  [[nodiscard]] double floor_attenuation_db() const { return floor_attenuation_db_; }

  [[nodiscard]] const std::vector<Level>& levels() const { return levels_; }
  [[nodiscard]] const Level& level(int index) const { return levels_[static_cast<std::size_t>(index)]; }
  [[nodiscard]] int level_count() const { return static_cast<int>(levels_.size()); }
  [[nodiscard]] bool multi_level() const { return levels_.size() > 1; }
  [[nodiscard]] std::optional<int> level_index(int ordinal) const;
  /// Index of the level whose floor elevation is closest to `z`.
  [[nodiscard]] int nearest_level(double z) const;

  [[nodiscard]] const std::vector<Connector>& connectors() const { return connectors_; }
  /// A connector serving `level` whose footprint contains `p`, or null.
  [[nodiscard]] const Connector* connector_at(Vec2 p, int level) const;

  [[nodiscard]] const std::vector<Room>& rooms() const { return rooms_; }
  [[nodiscard]] std::optional<std::size_t> room_index(std::string_view id) const;
  /// Indices of the rooms on `level` that may contain `p` (bounding-box grid
  /// cell); callers still test Room::contains. Valid until the venue changes.
  [[nodiscard]] std::span<const std::size_t> room_candidates(Vec2 p, int level) const;
  /// Indices of the rooms on `level` containing `p`, ascending, into `out` (cleared first).
  void rooms_at(Vec2 p, int level, std::vector<std::size_t>& out) const;

  [[nodiscard]] const std::optional<GeoRef>& georef() const { return georef_; }

  [[nodiscard]] std::optional<std::size_t> beacon_index(std::string_view id) const;

  /// True if moving in a straight line from a to b on `level` crosses any wall.
  [[nodiscard]] bool crosses_wall(Vec2 a, Vec2 b, int level = 0) const;

  /// Number of walls on `level` intersected by the straight line a-b (radio path).
  [[nodiscard]] int count_wall_crossings(Vec2 a, Vec2 b, int level = 0) const;

 private:
  /// Walls bucketed into a uniform grid so path queries only test nearby walls.
  struct Grid {
    double cell = 2.0;
    int nx = 0;
    int ny = 0;
    std::vector<std::vector<std::size_t>> cells;
  };

  /// Rooms bucketed by bounding box, one grid per level.
  struct RoomGrid {
    double cell = 5.0;
    int nx = 0;
    int ny = 0;
    std::vector<std::vector<std::size_t>> cells;
  };

  void validate() const;
  void build_grids();
  void build_room_grids();
  template <typename Fn>
  void for_each_candidate_wall(Vec2 a, Vec2 b, int level, Fn&& fn) const;

  std::string name_;
  Bounds bounds_{};
  Polygon footprint_;
  std::vector<Level> levels_;
  std::vector<Beacon> beacons_;
  std::vector<Connector> connectors_;
  std::vector<Room> rooms_;
  double wall_attenuation_db_ = 3.0;
  double floor_attenuation_db_ = 15.0;
  std::optional<GeoRef> georef_;
  std::vector<Grid> grids_;
  std::vector<RoomGrid> room_grids_;
};

/// Parses the venue JSON format documented in docs/formats.md.
Venue parse_venue(std::string_view json_text);
Venue load_venue_file(const std::string& path);

}  // namespace ipe
