#pragma once

#include <cstdint>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/filter.hpp"
#include "ipe/rng.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct ParticleFilterConfig {
  int num_particles = 1500;
  std::uint64_t seed = 1;

  double step_length_sigma = 0.08;     ///< Additive step length noise [m].
  double step_scale_sigma = 0.15;      ///< Initial spread of the per-user step scale.
  double step_scale_walk = 0.005;      ///< Random walk of the step scale per step.
  double heading_sigma_deg = 3.0;      ///< Heading noise added per step.
  double heading_outlier_prob = 0.15;  ///< Magnetic disturbances are frequent indoors.

  bool wall_constraint = true;  ///< Kill particles whose step crosses a wall.

  bool estimate_rssi_offset = true;  ///< Track a per-device RSSI bias as a state.
  double rssi_offset_sigma = 8.0;    ///< Prior spread of the device RSSI offset [dB].
  double rssi_offset_walk = 0.05;    ///< Random walk of the offset per RSSI update [dB].

  /// Beacon integrity monitor: once converged, track each beacon's mean RSSI
  /// residual at the estimate and stop using beacons that disagree for long
  /// (moved or mis-surveyed beacons). Hysteresis re-admits them.
  bool beacon_integrity = true;
  double integrity_threshold_db = 10.0;
  double integrity_alpha = 0.1;
  int integrity_min_samples = 8;
  double integrity_max_sigma_m = 3.0;

  double resample_ess_ratio = 0.5;  ///< Resample when ESS < ratio * N.
  double roughening_m = 0.15;       ///< Position jitter applied after resampling.

  /// Augmented MCL: inject uniformly drawn particles when the short-term
  /// measurement likelihood drops well below the long-term average.
  bool recovery = true;
  double recovery_alpha_slow = 0.02;
  double recovery_alpha_fast = 0.05;
  double recovery_max_fraction = 0.05;

  // Floor model; only active in multi-level venues.
  double level_change_prob_step = 0.08;  ///< Per step, for particles standing in a connector footprint.
  double level_change_prob_baro = 0.1;   ///< Per barometer update in a connector (lift rides have no steps).
  bool use_barometer = true;
  double baro_update_interval_s = 0.5;
  double baro_sigma_m = 0.8;  ///< Altitude vs. floor elevation spread (stairs, phone height, residual drift).
  double baro_outlier_prob = 0.05;
  double baro_offset_sigma_m = 0.3;  ///< Spread of a new particle's altitude offset.
  double baro_offset_walk_m = 0.05;  ///< Random walk of the offset per barometer update (weather drift).
};

struct Particle {
  Vec2 pos;
  double heading = 0.0;
  double step_scale = 1.0;
  double rssi_offset = 0.0;
  double weight = 0.0;
  int level = 0;             ///< Level index.
  double baro_offset = 0.0;  ///< Barometric altitude minus the floor elevation [m].
};

/// Sequential importance resampling filter fusing PDR steps, magnetometer
/// heading, BLE RSSI and barometric altitude, with the floor plan used both as
/// a motion constraint and inside the radio likelihood. In multi-level venues
/// every particle carries a level; it can only change level inside a
/// stairs/lift footprint, and the barometer and the slab attenuation in the
/// radio model decide which level hypotheses survive.
class ParticleFilter final : public Filter {
 public:
  ParticleFilter(const Venue& venue, ParticleFilterConfig cfg, RssiModelConfig rssi_cfg, BaroConfig baro_cfg = {});

  void on_step(const StepEvent& step) override;
  void on_heading(double t, double heading, double sigma) override;
  void on_rssi(double t, std::size_t beacon_index, double rssi) override;
  void on_pressure(double t, double hpa) override;

  [[nodiscard]] Estimate estimate() const override { return estimate_; }
  [[nodiscard]] std::string_view name() const override { return "pf"; }

  [[nodiscard]] const std::vector<Particle>& particles() const { return particles_; }
  [[nodiscard]] double effective_sample_size() const;
  [[nodiscard]] std::uint64_t resample_count() const { return resample_count_; }
  [[nodiscard]] std::uint64_t recovery_count() const { return recovery_count_; }
  [[nodiscard]] bool beacon_excluded(std::size_t beacon_index) const;

 private:
  struct BeaconHealth {
    double mean_residual = 0.0;
    int samples = 0;
    bool excluded = false;
  };

  /// Updates the integrity monitor; returns false if the beacon should be skipped.
  bool check_beacon(std::size_t beacon_index, double rssi);

  void initialize_uniform();
  Particle sample_uniform_particle();
  /// Moves a particle standing in a connector to an adjacent level with probability `prob`.
  void maybe_change_level(Particle& p, double prob);
  [[nodiscard]] double elevation(int level) const { return venue_->level(level).elevation_m; }
  /// Multiplies weights by exp(loglik - max) and renormalises. Returns false
  /// if every particle ended with zero weight.
  bool apply_log_likelihoods(const std::vector<double>& loglik);
  void normalize_or_recover();
  void resample_if_needed();
  void update_estimate(double t);

  const Venue* venue_;
  ParticleFilterConfig cfg_;
  RssiModel rssi_model_;
  Rng rng_;
  std::vector<Particle> particles_;
  std::vector<Particle> scratch_;
  std::vector<double> loglik_;
  std::vector<BeaconHealth> health_;
  double mean_rssi_offset_ = 0.0;
  Estimate estimate_;

  double w_slow_ = 0.0;
  double w_fast_ = 0.0;
  std::uint64_t resample_count_ = 0;
  std::uint64_t recovery_count_ = 0;

  bool multi_level_ = false;
  BaroAltimeter altimeter_;
  bool baro_ready_ = false;  ///< Particle altitude offsets have been initialised.
  double last_baro_update_t_ = -1e300;
  std::vector<double> level_weight_;
};

}  // namespace ipe
