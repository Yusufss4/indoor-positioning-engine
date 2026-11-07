#pragma once

#include <cstddef>

#include "ipe/geometry.hpp"
#include "ipe/pdr.hpp"

namespace ipe {

struct Estimate {
  double t = 0.0;
  Vec2 pos;
  double heading = 0.0;
  double cov_xx = 0.0;
  double cov_xy = 0.0;
  double cov_yy = 0.0;
  int level = 0;   ///< Level index in the venue.
  double z = 0.0;  ///< Floor elevation of `level` [m].
  bool valid = false;

  /// Radius of a circle with the same area as the 1-sigma ellipse.
  [[nodiscard]] double sigma_radius() const;
};

struct RssiReading {
  std::size_t beacon_index = 0;
  double rssi = 0.0;
};

}  // namespace ipe
