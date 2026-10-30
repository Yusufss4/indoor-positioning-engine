#pragma once

#include <array>
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

  /// Beacon-driven proposal: the first RSSI packet re-seeds the cloud from a
  /// mixture of a range ring around that beacon and the uniform prior, with
  /// importance weights so the result is still the exact posterior.
  bool init_proposal = false;
  double init_proposal_fraction = 0.8;  ///< Share of particles drawn from the ring.
  double init_window_s = 1.0;           ///< Packets collected before seeding; the closest-looking beacon wins.
  /// Recovery injections are drawn from the ring of a recent packet instead
  /// of uniformly over the venue.
  bool recovery_proposal = false;
  int proposal_history = 8;          ///< Recent packets remembered (at most kMaxProposalHistory).
  double proposal_max_age_s = 2.0;   ///< Older packets are not used for recovery proposals.
  double proposal_level_jump = 0.1;  ///< Probability a ring particle is put on a level next to the beacon's.
  double proposal_ring_scale = 1.0;  ///< Ring width in units of the RSSI model's sigma.

  /// A scan of k readings multiplies their likelihoods raised to k^-gamma.
  /// 0 treats readings as independent; 1 counts the whole scan as one measurement.
  double scan_tempering = 0.0;

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
  static constexpr int kMaxProposalHistory = 32;

  ParticleFilter(const Venue& venue, ParticleFilterConfig cfg, RssiModelConfig rssi_cfg, BaroConfig baro_cfg = {});

  void on_step(const StepEvent& step) override;
  void on_heading(double t, double heading, double sigma) override;
  void on_rssi(double t, std::size_t beacon_index, double rssi) override;
  /// One weight update, resample and estimate for the whole scan, with the
  /// readings' summed log-likelihood tempered by `scan_tempering`.
  void on_rssi_scan(double t, std::span<const RssiReading> readings) override;
  void on_pressure(double t, double hpa) override;

  [[nodiscard]] Estimate estimate() const override { return estimate_; }
  [[nodiscard]] std::string_view name() const override { return "pf"; }

  [[nodiscard]] const std::vector<Particle>& particles() const { return particles_; }
  [[nodiscard]] double effective_sample_size() const;
  [[nodiscard]] std::uint64_t resample_count() const { return resample_count_; }
  [[nodiscard]] std::uint64_t recovery_count() const { return recovery_count_; }
  [[nodiscard]] bool beacon_excluded(std::size_t beacon_index) const;
  [[nodiscard]] bool radio_seeded() const { return radio_seeded_; }

  /// Replaces the cloud with draws from the init proposal for one packet and
  /// importance-weights them back to the prior. The packet's likelihood is not
  /// applied; on_rssi does that afterwards. Called by the first on_rssi when
  /// `init_proposal` is on.
  void seed_from_beacon(std::size_t beacon_index, double rssi);

  /// Range ring around a beacon: log10 of the distance is normal with the given
  /// mean and spread, the bearing uniform. Returns the 2D density at offset `d`
  /// metres from the beacon [1/m^2].
  [[nodiscard]] static double ring_density(double d, double log10_mean, double log10_sigma);

 private:
  struct BeaconHealth {
    double mean_residual = 0.0;
    int samples = 0;
    bool excluded = false;
  };

  struct Packet {
    double t = 0.0;
    std::size_t beacon = 0;
    double rssi = 0.0;
  };

  struct Ring {
    double log10_mean = 0.0;
    double log10_sigma = 1.0;
  };

  struct RingDraw {
    int attempts = 0;
    bool fell_back = false;  ///< Every try left the venue; the position is uniform.
  };

  static constexpr int kRingTries = 4;
  static constexpr int kFootprintTries = 64;  ///< Enough for a footprint covering 10% of the bounds.

  [[nodiscard]] Ring ring_for(const Beacon& beacon, double rssi, double rssi_offset) const;
  /// Draws a particle whose device offset and distance from `beacon` are
  /// consistent with hearing it at `rssi`.
  Particle sample_ring_particle(const Beacon& beacon, double rssi, RingDraw& draw);
  /// Probability the ring proposal puts a particle on `level`.
  [[nodiscard]] double ring_level_prob(int beacon_level, int level) const;
  Particle sample_recovery_particle();
  void remember_packet(double t, std::size_t beacon_index, double rssi);
  /// 10 log10 of the free-space distance at which the beacon is heard at `rssi`.
  [[nodiscard]] double apparent_range(std::size_t beacon_index, double rssi) const;
  /// Applies the packets collected before the radio seed.
  void flush_pending();
  /// RSSI likelihood update, after the integrity check.
  void update_rssi(double t, std::size_t beacon_index, double rssi);
  void update_rssi(double t, std::span<const RssiReading> readings);

  /// Updates the integrity monitor; returns false if the beacon should be skipped.
  bool check_beacon(std::size_t beacon_index, double rssi);

  void initialize_uniform();
  /// Uniform over the venue's walkable area (its footprint, else its bounds).
  Vec2 sample_position();
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
  std::vector<RssiReading> scan_;  ///< Readings of the current scan that passed the integrity check.
  std::vector<BeaconHealth> health_;
  double mean_rssi_offset_ = 0.0;
  Estimate estimate_;

  double w_slow_ = 0.0;
  double w_fast_ = 0.0;
  double now_ = 0.0;
  bool radio_seeded_ = false;
  /// The estimate stays invalid until radio has been applied: the uniform
  /// prior's mean and spread are not a position.
  bool localized_ = false;
  std::array<Packet, kMaxProposalHistory> history_{};
  std::size_t history_head_ = 0;
  std::size_t history_count_ = 0;
  std::array<Packet, kMaxProposalHistory> pending_{};  ///< Packets collected before the radio seed.
  std::size_t pending_count_ = 0;
  std::uint64_t resample_count_ = 0;
  std::uint64_t recovery_count_ = 0;

  bool multi_level_ = false;
  BaroAltimeter altimeter_;
  bool baro_ready_ = false;  ///< Particle altitude offsets have been initialised.
  double last_baro_update_t_ = -1e300;
  std::vector<double> level_weight_;
};

}  // namespace ipe
