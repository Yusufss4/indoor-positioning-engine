#pragma once

#include <cstdint>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// The part of each level a walker can be in. The walls split a level into connected regions, and a
/// walker never moves from one to another except through a stairwell or a lift. A region is *closed* if it
/// is at least `min_m2` in area, smaller than `max_fraction` of the level's largest region and holds no
/// connector: a room without a door, a solid island drawn as an outline. Nobody can walk into one, so no
/// hypothesis should start there. Smaller pockets (gaps between double walls, shelf slots) are left as
/// they are.
///
/// Built once from a grid of `kCellM` cells (coarser for huge levels), linking neighbouring cells whose
/// centres see each other past the walls; one byte per cell is kept. O(cells) to build, O(1) per query
/// away from walls and a few wall tests next to one.
class ReachableArea {
 public:
  ReachableArea(const Venue& venue, double max_fraction, double min_m2);

  /// True if some level has a closed region.
  [[nodiscard]] bool any_closed() const { return any_closed_; }
  /// False if `p` lies in a closed region of `level`; true anywhere else, off the map included.
  [[nodiscard]] bool open(Vec2 p, int level) const;
  /// Closed area of `level` [m^2].
  [[nodiscard]] double closed_area(int level) const { return levels_[static_cast<std::size_t>(level)].closed_m2; }

 private:
  static constexpr double kCellM = 0.25;
  static constexpr double kMaxCells = 4e6;  ///< Per level; coarser cells beyond that.
  enum State : std::uint8_t { kOff = 0, kOpen = 1, kClosed = 2 };

  struct LevelGrid {
    std::vector<std::uint8_t> state;  ///< State of the region holding each cell's centre.
    double closed_m2 = 0.0;
  };

  [[nodiscard]] Vec2 centre(int x, int y) const;
  void build_level(int level, double max_fraction, double min_m2);

  const Venue* venue_;
  double cell_m_ = kCellM;
  int nx_ = 1;
  int ny_ = 1;
  std::vector<LevelGrid> levels_;
  bool any_closed_ = false;
};

}  // namespace ipe
