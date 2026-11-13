#pragma once

#include "ipe/geometry.hpp"

namespace ipe {

/// A point of the posterior with its share of the mass: a grid cell's mass at its centroid.
struct WeightedPoint {
  Vec2 pos;
  double weight = 0.0;
  int level = 0;  ///< Level index.
};

}  // namespace ipe
