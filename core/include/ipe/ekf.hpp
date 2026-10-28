#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ipe/filter.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct EkfConfig {
  double step_length_sigma_rel = 0.10;  ///< Step length noise as a fraction of L.
  double step_length_sigma_abs = 0.05;
  double heading_sigma_deg = 3.0;  ///< Heading process noise per step.
  double init_pos_sigma = 5.0;
  double init_heading_sigma_deg = 60.0;
  /// Weak packets mostly come from far beacons behind walls, where the
  /// free-space model is badly biased and the linearisation is flat.
  double rssi_min_dbm = -85.0;
  double rssi_gate_chi2 = 9.0;      ///< Innovation gate for RSSI (3 sigma).
  double heading_gate_chi2 = 6.63;  ///< Innovation gate for heading (99%).
  int init_min_beacons = 3;         ///< Distinct beacons needed before initialising.
  double init_window_s = 2.0;
  int reinit_after_rejections = 25;  ///< Consecutive gated RSSI before re-initialising.
};

/// Extended Kalman filter on the state [x, y, heading]. Steps drive the
/// prediction; magnetometer heading and per-beacon RSSI are scalar updates.
/// It has no notion of walls: it is the map-free baseline the particle
/// filter is compared against.
class Ekf final : public Filter {
 public:
  using Mat3 = std::array<double, 9>;

  Ekf(const Venue& venue, EkfConfig cfg, RssiModelConfig rssi_cfg);

  void on_step(const StepEvent& step) override;
  void on_heading(double t, double heading, double sigma) override;
  void on_rssi(double t, std::size_t beacon_index, double rssi) override;

  [[nodiscard]] Estimate estimate() const override;
  [[nodiscard]] std::string_view name() const override { return "ekf"; }

  [[nodiscard]] const std::array<double, 3>& state() const { return x_; }
  [[nodiscard]] const Mat3& covariance() const { return P_; }
  [[nodiscard]] std::uint64_t rejected_updates() const { return rejected_total_; }

 private:
  struct Pending {
    double t;
    std::size_t beacon;
    double rssi;
  };

  void try_initialize(double t);
  /// Scalar measurement update with Jacobian H. Returns false if gated.
  bool scalar_update(const std::array<double, 3>& H, double innovation, double R, double gate_chi2);

  const Venue* venue_;
  EkfConfig cfg_;
  RssiModel rssi_model_;

  bool initialized_ = false;
  bool heading_known_ = false;
  double last_heading_ = 0.0;
  double last_t_ = 0.0;
  std::array<double, 3> x_{};
  Mat3 P_{};
  std::vector<Pending> pending_;
  int consecutive_rejections_ = 0;
  std::uint64_t rejected_total_ = 0;
};

}  // namespace ipe
