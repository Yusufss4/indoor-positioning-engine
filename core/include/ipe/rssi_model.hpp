#pragma once

#include "ipe/venue.hpp"

namespace ipe {

struct RssiModelConfig {
  double sigma_db = 5.0;           ///< Gaussian spread of RSSI around the path-loss curve.
  double outlier_prob = 0.05;      ///< Mixture weight of the uniform outlier component.
  double outlier_range_db = 60.0;  ///< Support of the uniform outlier component.
  double min_distance = 0.5;       ///< Distances are clamped to avoid log(0) near beacons.
  bool wall_aware = true;          ///< Subtract per-wall attenuation (needs the map).
};

/// Log-distance path-loss model with an optional per-wall attenuation term
/// and a robust (Gaussian + uniform) measurement likelihood.
class RssiModel {
 public:
  RssiModel(const Venue& venue, RssiModelConfig cfg = {}) : venue_(&venue), cfg_(cfg) {}

  [[nodiscard]] double expected_rssi(const Beacon& b, Vec2 p) const;
  [[nodiscard]] double expected_rssi_free_space(const Beacon& b, double d) const;
  [[nodiscard]] double distance_from_rssi(const Beacon& b, double rssi) const;
  [[nodiscard]] double log_likelihood(double residual_db) const;

  [[nodiscard]] const RssiModelConfig& config() const { return cfg_; }

 private:
  const Venue* venue_;
  RssiModelConfig cfg_;
};

}  // namespace ipe
