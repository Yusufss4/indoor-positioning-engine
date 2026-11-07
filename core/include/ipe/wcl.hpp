#pragma once

#include <deque>

#include "ipe/filter.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct WclConfig {
  double window_s = 3.0;  ///< Sliding window used to average RSSI per beacon.
  int top_k = 4;          ///< Number of strongest beacons in the centroid.
  double weight_exponent = 2.0;
  /// Report a position that follows the centroid without crossing walls (see MapFollower).
  bool wall_constraint = false;
  double wall_escape_s = 3.0;  ///< How long the centroid may stay behind a wall before the estimate jumps to it [s].
};

/// Weighted centroid localisation: the simplest RSSI-only baseline. Ignores
/// steps, heading and the barometer entirely. In multi-level venues the floor
/// is a weighted vote of the strongest beacons' levels. Map-free unless
/// `wall_constraint` is set.
class WeightedCentroid final : public Filter {
 public:
  WeightedCentroid(const Venue& venue, WclConfig cfg, RssiModelConfig rssi_cfg);

  void on_step(const StepEvent&) override {}
  void on_heading(double, double, double) override {}
  void on_rssi(double t, std::size_t beacon_index, double rssi) override;

  [[nodiscard]] Estimate estimate() const override { return estimate_; }
  [[nodiscard]] std::string_view name() const override { return "wcl"; }
  [[nodiscard]] const MapFollower& map_follower() const { return follower_; }

 private:
  struct Sample {
    double t;
    std::size_t beacon;
    double rssi;
  };

  const Venue* venue_;
  WclConfig cfg_;
  RssiModel rssi_model_;
  std::deque<Sample> window_;
  Estimate estimate_;
  MapFollower follower_;
};

}  // namespace ipe
