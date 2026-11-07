#pragma once

#include "ipe/geometry.hpp"

namespace ipe {

/// One hypothesis of the particle filter.
struct Particle {
  Vec2 pos;
  double heading = 0.0;
  double step_scale = 1.0;
  double rssi_offset = 0.0;
  double weight = 0.0;
  int level = 0;             ///< Level index.
  double baro_offset = 0.0;  ///< Barometric altitude minus the floor elevation [m].
};

}  // namespace ipe
