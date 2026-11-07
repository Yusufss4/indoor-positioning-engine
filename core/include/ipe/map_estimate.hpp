#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "ipe/particle.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// Keeps a reported position on the map. The weighted mean of a particle
/// cloud split across a wall can fall on the wall, in an empty room or behind
/// a wall from every particle; then the mean of the dominant group is used
/// instead: the heaviest 3x3 block of a coarse weight histogram, keeping only
/// the block's particles on the same side of the walls as the one nearest the
/// block's centre. O(N) per call; the histogram is allocated once.
class MapConsistentEstimate {
 public:
  /// `particle_capacity` sizes the touched-cell list so calls do not allocate.
  MapConsistentEstimate(const Venue& venue, std::size_t particle_capacity);

  /// `mean` if it is walkable and in sight of its nearest particle on `level`,
  /// else the dominant group's mean (or a particle, if even that is off the map).
  /// With `multi_level`, only particles on `level` count.
  Vec2 position(std::span<const Particle> particles, Vec2 mean, int level, bool multi_level);

 private:
  static constexpr double kCellM = 2.0;
  [[nodiscard]] std::size_t cell(Vec2 p) const;

  const Venue* venue_;
  double cell_m_ = kCellM;
  int nx_ = 1;
  int ny_ = 1;
  std::vector<double> weight_;
  std::vector<std::size_t> touched_;  ///< Cells with weight; only these are cleared after a call.
};

}  // namespace ipe
