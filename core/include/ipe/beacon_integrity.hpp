#pragma once

#include <cstddef>
#include <vector>

namespace ipe {

/// Tracks each beacon's mean RSSI residual at the converged estimate and stops
/// using beacons that disagree for long (moved or mis-surveyed beacons).
/// Hysteresis re-admits them, and at most a third of the beacons are ever
/// excluded at once.
class BeaconIntegrityMonitor {
 public:
  struct Settings {
    double threshold_db = 10.0;  ///< Exclude above this mean residual; re-admit below half of it.
    double alpha = 0.1;          ///< Smoothing of the mean residual.
    int min_samples = 8;         ///< Residuals needed before a beacon can be excluded.
    double max_sigma_m = 3.0;    ///< Residuals are only judged when the estimate is this tight.
  };

  BeaconIntegrityMonitor(std::size_t beacon_count, Settings settings);

  /// True if an estimate with this 1-sigma radius is tight enough to judge residuals at.
  [[nodiscard]] bool can_judge(double sigma_radius) const { return !(sigma_radius > settings_.max_sigma_m); }
  /// Records `residual` (measured minus predicted RSSI at the estimate) for
  /// `beacon` and returns whether the beacon may be used.
  bool admit(std::size_t beacon, double residual);
  [[nodiscard]] bool excluded(std::size_t beacon) const { return beacon < health_.size() && health_[beacon].excluded; }

 private:
  struct Health {
    double mean_residual = 0.0;
    int samples = 0;
    bool excluded = false;
  };

  Settings settings_;
  std::vector<Health> health_;
};

}  // namespace ipe
