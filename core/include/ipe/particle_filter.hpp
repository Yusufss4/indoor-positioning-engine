#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/beacon_integrity.hpp"
#include "ipe/filter.hpp"
#include "ipe/fingerprint_map.hpp"
#include "ipe/floor_classifier.hpp"
#include "ipe/level_selector.hpp"
#include "ipe/map_estimate.hpp"
#include "ipe/particle.hpp"
#include "ipe/reachable_area.hpp"
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

  /// The cloud mean can fall where no particle can be (on a wall, in an empty
  /// shop between two groups). Report the dominant group's mean instead then;
  /// the covariance still covers the whole cloud.
  bool map_consistent_estimate = true;
  /// Rooms nobody can walk into. The walls split each level into connected regions; one of at least
  /// `closed_region_min_m2`, smaller than `closed_region_fraction` of its level's largest region and
  /// without a stairwell or lift is closed (a room without a door, an island drawn as an outline). No
  /// particle is placed in one: not by the uniform start, a beacon ring, recovery or a re-seed. Steps
  /// cannot enter one anyway. 0 turns it off; a venue without a closed region is unaffected.
  double closed_region_fraction = 0.1;
  double closed_region_min_m2 = 10.0;
  /// The reported level follows the level weights averaged with time constant `level_smoothing_s`, and
  /// only moves once another level leads the reported one by `level_hysteresis` in that average, so a
  /// cloud split between floors does not flicker with every packet. Both 0 report the most likely level
  /// of every update. The filter's own decisions (integrity, recovery, barometer) always use the most
  /// likely level.
  double level_smoothing_s = 0.5;
  double level_hysteresis = 0.1;

  bool estimate_rssi_offset = true;  ///< Track a per-device RSSI bias as a state.
  double rssi_offset_sigma = 8.0;    ///< Prior spread of the device RSSI offset [dB].
  double rssi_offset_walk = 0.05;    ///< Random walk of the offset per RSSI update [dB].

  /// Online calibration of the loss per slab (multi-level venues). Once the estimate has converged (the
  /// integrity monitor's test), each admitted reading from a beacon on another level nudges one venue-wide
  /// slab loss by `loss_calibration_rate` times its residual at the estimate (per slab, clipped to
  /// `loss_calibration_clip_db`). One shared value, not a particle state: a per-particle loss lets a
  /// wrong position with a matching loss explain the readings as well as the right one (experiment 10).
  bool calibrate_floor_loss = false;
  double loss_calibration_rate = 0.02;
  double loss_calibration_clip_db = 15.0;
  /// Readings predicted weaker than this are not used: receivers drop packets near their sensitivity, so
  /// the weak readings that do arrive are biased strong and would make the loss look smaller.
  double loss_calibration_min_rssi_dbm = -85.0;

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
  /// With `init_proposal` off, turn it on anyway when the cloud is sparse: fewer than this many particles
  /// per m^2 of walkable area (all levels together). A uniform start samples a small walled office well,
  /// but not a hall or a mall floor. 0 never turns it on.
  double init_proposal_auto_density = 0.6;
  double init_proposal_fraction = 0.8;  ///< Share of particles drawn from the ring.
  double init_window_s = 1.0;           ///< Packets collected before seeding; the closest-looking beacon wins.
  /// Recovery injections are drawn from the ring of a recent packet instead
  /// of uniformly over the venue.
  bool recovery_proposal = false;
  int proposal_history = 8;          ///< Recent packets remembered (at most kMaxProposalHistory).
  double proposal_max_age_s = 2.0;   ///< Older packets are not used for recovery proposals.
  double proposal_level_jump = 0.1;  ///< Probability a ring particle is put on a level next to the beacon's.

  // Floor model; only active in multi-level venues.
  double level_change_prob_step = 0.08;  ///< Per step, for particles standing in a connector footprint.
  double level_change_prob_baro = 0.1;   ///< Per barometer update in a connector (lift rides have no steps).
  double baro_update_interval_s = 0.5;
  double baro_sigma_m = 0.8;  ///< Altitude vs. floor elevation spread (stairs, phone height, residual drift).
  double baro_outlier_prob = 0.05;
  double baro_offset_sigma_m = 0.3;  ///< Spread of a new particle's altitude offset.
  double baro_offset_walk_m = 0.05;  ///< Random walk of the offset per barometer update (weather drift).
  /// Barometer-driven floor change: once the cloud's altitude residual has pointed at an adjacent level
  /// (more than half the gap) for `baro_jump_hold_s`, particles whose own residual fits that level better
  /// move onto it at the nearest connector within `baro_jump_radius_m`, with `baro_jump_prob` per update.
  /// Covers stairs taken while the position estimate is still metres away from them.
  bool baro_jump = false;
  double baro_jump_radius_m = 25.0;
  double baro_jump_hold_s = 3.0;
  double baro_jump_prob = 0.1;
  /// Share of new particles (recovery, ring proposals, re-seeds) that take the cloud's altitude reference
  /// instead of one made to fit their own level, so the barometer can reject a re-seed on the wrong floor.
  /// 0 keeps every new particle consistent with the current reading.
  double baro_reseed_keep_reference = 0.0;
  /// Floor prior from a k-NN classifier over the venue's floor fingerprints, applied per Wi-Fi scan as
  /// `floor_prior_weight * log p(level)`. 0 (or a venue without fingerprints) turns it off.
  double floor_prior_weight = 0.0;
  int floor_prior_k = 5;
  double floor_prior_eps = 0.1;
  /// Recovery injections take their level from the last classified scan if it is at most this old.
  double floor_prior_max_age_s = 5.0;

  /// Fingerprint likelihood, for venues whose fingerprints carry positions (training scans). Each scan's
  /// `fingerprint_k` nearest training scans, compared over its `fingerprint_strongest` strongest readings,
  /// form a kernel density over position and level: Gaussians of `fingerprint_sigma_m` around the
  /// neighbours, mixed with `fingerprint_eps` of the uniform prior. It is applied per scan as
  /// `fingerprint_weight * log density`, next to the log-distance model. 0 (or a venue without such
  /// fingerprints) turns it off.
  double fingerprint_weight = 0.0;
  int fingerprint_k = 10;
  int fingerprint_strongest = 20;
  double fingerprint_sigma_m = 6.0;
  double fingerprint_eps = 0.05;
  /// With the fingerprint likelihood on, the first scan seeds the cloud from that density (instead of a
  /// beacon ring), and recovery injections are drawn from the last scan's if it is at most
  /// `fingerprint_max_age_s` old.
  bool fingerprint_proposal = true;
  double fingerprint_max_age_s = 5.0;
  /// Scale of the log-distance model's log-likelihood in every RSSI update. Below 1 it counts for less
  /// next to the fingerprints (a scan's readings are not independent); 0 leaves radio to the fingerprints.
  double rssi_weight = 1.0;
};

/// Sequential importance resampling filter fusing PDR steps, magnetometer
/// heading, BLE RSSI and barometric altitude, with the floor plan used both as
/// a motion constraint and inside the radio likelihood. In multi-level venues
/// every particle carries a level; it can only change level inside a
/// stairs/lift footprint, and the barometer and the slab attenuation in the
/// radio model decide which level hypotheses survive.
class ParticleFilter {
 public:
  static constexpr int kMaxProposalHistory = 32;

  ParticleFilter(const Venue& venue, ParticleFilterConfig cfg, RssiModelConfig rssi_cfg, BaroConfig baro_cfg = {});

  void on_step(const StepEvent& step);
  void on_heading(double t, double heading, double sigma);
  void on_rssi(double t, std::size_t beacon_index, double rssi);
  /// One weight update, resample and estimate for the whole scan.
  void on_rssi_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);

  [[nodiscard]] Estimate estimate() const { return estimate_; }
  /// Displacement of the last step itself: the weighted mean move of the particles that survived it. The
  /// estimate's change across the step also holds corrections (particles dying at walls, resampling).
  [[nodiscard]] Vec2 step_motion() const { return step_motion_; }

  [[nodiscard]] const std::vector<Particle>& particles() const { return particles_; }
  [[nodiscard]] double effective_sample_size() const;
  [[nodiscard]] std::uint64_t resample_count() const { return resample_count_; }
  [[nodiscard]] std::uint64_t recovery_count() const { return recovery_count_; }
  [[nodiscard]] bool beacon_excluded(std::size_t beacon_index) const;
  [[nodiscard]] bool radio_seeded() const { return radio_seeded_; }
  /// Loss per slab in use: the venue's, or the calibrated value.
  [[nodiscard]] double floor_loss_db() const { return floor_db_; }
  /// Whether the init proposal is in use (`init_proposal`, or chosen by `init_proposal_auto_density`).
  [[nodiscard]] bool init_proposal_active() const { return cfg_.init_proposal; }
  /// Whether the venue has closed regions that sampling avoids (see `closed_region_fraction`).
  [[nodiscard]] bool avoids_closed_regions() const { return reachable_.has_value(); }

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
    int attempts = 0;        ///< Tries: draws that left the venue, or were accepted.
    int draws = 0;           ///< Every draw, the ones redrawn out of a closed region too.
    bool fell_back = false;  ///< Every try left the venue; the position is uniform.
  };

  static constexpr int kRingTries = 4;
  /// Ring draws in a closed region per particle that are drawn again without using up a try.
  static constexpr int kClosedRedraws = 16;
  static constexpr double kMaxFloorDb = 40.0;
  static constexpr int kFootprintTries = 64;   ///< Enough for a footprint covering 10% of the bounds.
  static constexpr int kFingerprintTries = 8;  ///< Draws around a fingerprint neighbour before giving up on it.

  [[nodiscard]] Ring ring_for(const Beacon& beacon, double rssi, double rssi_offset) const;
  /// Draws a particle whose device offset and distance from `beacon` are
  /// consistent with hearing it at `rssi`.
  Particle sample_ring_particle(const Beacon& beacon, double rssi, RingDraw& draw);
  /// Probability the ring proposal puts a particle on `level`.
  [[nodiscard]] double ring_level_prob(int beacon_level, int level) const;
  Particle sample_recovery_particle();
  /// Density [1/m^2] of the current scan's fingerprint neighbours at `pos` on `level`.
  [[nodiscard]] double fingerprint_density(Vec2 pos, int level) const;
  /// A particle near one of the current scan's fingerprint neighbours, drawn by their weights.
  Particle sample_fingerprint_particle();
  /// Replaces the cloud with draws from the fingerprint density mixed with the uniform prior, importance-
  /// weighted back to the prior; the scan's likelihood is applied afterwards.
  void seed_from_fingerprints();
  [[nodiscard]] bool fingerprint_proposal_fresh() const {
    return cfg_.fingerprint_proposal && neighbour_count_ > 0 &&
           now_ - last_fingerprint_t_ <= cfg_.fingerprint_max_age_s;
  }
  void remember_packet(double t, std::size_t beacon_index, double rssi);
  /// 10 log10 of the free-space distance at which the beacon is heard at `rssi`.
  [[nodiscard]] double apparent_range(std::size_t beacon_index, double rssi) const;
  /// Applies the packets collected before the radio seed.
  void flush_pending();
  /// RSSI likelihood update, after the integrity check.
  void update_rssi(double t, std::size_t beacon_index, double rssi);
  void update_rssi(double t, std::span<const RssiReading> readings);

  /// Updates the integrity monitor and the loss calibration; returns false if the beacon should be skipped.
  bool check_beacon(std::size_t beacon_index, double rssi);
  /// One slab-loss calibration step from a reading's residual at the converged estimate.
  void calibrate_floor_loss(const Beacon& beacon, double residual);

  void initialize_uniform();
  /// Uniform over the venue's walkable area (its footprint, else its bounds);
  /// with `level` >= 0, over that level's footprint if it has one. Never in a
  /// closed region of `open_level` (default: `level`, or 0 for none).
  Vec2 sample_position(int level = -1, int open_level = -1);
  /// A level index with probability proportional to its walkable area (closed regions left out).
  int sample_level_by_area();
  /// Walkable area of `level` that a particle may be placed in: closed regions left out.
  [[nodiscard]] double placeable_area(int level) const;
  /// True if a particle may be placed at `p` on `level`: inside the venue (and the level's footprint)
  /// and not in a closed region.
  [[nodiscard]] bool placeable(Vec2 p, int level) const {
    return venue_->inside(p, level) && (!reachable_ || reachable_->open(p, std::max(level, 0)));
  }
  Particle sample_uniform_particle();
  /// Moves a particle standing in a connector to an adjacent level with probability `prob`.
  void maybe_change_level(Particle& p, double prob);
  /// Arms the barometer jump when the cloud's residual has pointed at an adjacent level for the hold time.
  void update_baro_drift(double t, double alt);
  /// Moves `p` to the adjacent level its barometer residual points at, inside the nearest connector.
  void maybe_baro_jump(Particle& p, double alt);
  /// Altitude offset for a new particle on `level` (see baro_reseed_keep_reference).
  double fresh_baro_offset(int level, bool jitter = true);
  [[nodiscard]] double elevation(int level) const { return venue_->level(level).elevation_m; }
  /// Multiplies weights by exp(loglik - max) and renormalises. Returns false
  /// if every particle ended with zero weight.
  bool apply_log_likelihoods(const std::vector<double>& loglik);
  /// Applies `loglik_` as one measurement update, then finish_update(t).
  void commit_log_likelihoods(double t);
  /// Normalises (re-seeding if every particle died), resamples if needed and updates the estimate.
  void finish_update(double t);
  void normalize_or_recover();
  void resample_if_needed();
  void update_estimate(double t);
  /// Weighted moments of the particles on `level` (all particles in a single-level venue).
  struct Moments {
    Vec2 mean;
    double sin_heading = 0.0, cos_heading = 0.0;
    double cxx = 0.0, cxy = 0.0, cyy = 0.0;
    double rssi_offset = 0.0, baro_offset = 0.0;
  };
  [[nodiscard]] Moments moments(int level) const;

  const Venue* venue_;
  ParticleFilterConfig cfg_;
  RssiModel rssi_model_;
  Rng rng_;
  std::vector<Particle> particles_;
  std::vector<Particle> scratch_;
  std::vector<double> loglik_;
  std::vector<RssiReading> scan_;  ///< Readings of the current scan that passed the integrity check.
  BeaconIntegrityMonitor integrity_;
  std::optional<MapConsistentEstimate> map_estimate_;  ///< Engaged when `map_consistent_estimate` is on.
  std::optional<ReachableArea> reachable_;             ///< Engaged when the venue has closed regions to avoid.
  double mean_rssi_offset_ = 0.0;
  double floor_db_ = 0.0;  ///< Loss per slab in the radio model (the venue's value until calibrated).
  [[nodiscard]] double expected_rssi(const Beacon& b, Vec2 pos, int level) const {
    return rssi_model_.expected_rssi(b, pos, level, venue_->wall_attenuation_db(), floor_db_);
  }
  Estimate estimate_;  ///< What is reported: the cloud mean, or its dominant mode (map_consistent_estimate).
  Estimate cloud_;     ///< Mean and covariance on the most likely level; what the filter's own decisions use.
  Vec2 step_motion_;   ///< See step_motion().

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
  double baro_drift_since_ = std::numeric_limits<double>::quiet_NaN();  ///< Start of the current drift.
  int baro_drift_dir_ = 0;                                              ///< +1 up, -1 down, 0 none.
  bool baro_jump_armed_ = false;
  double baro_reference_ = 0.0;  ///< Weighted mean altitude offset of the particles on the estimated level.
  bool baro_reference_valid_ = false;
  FloorClassifier floor_classifier_;
  std::vector<double> floor_p_;  ///< Level probabilities of the current scan.
  bool floor_prior_active_ = false;
  double last_floor_t_ = -1e300;  ///< Time of the last classified scan (floor_p_ holds its output).
  FingerprintMap fingerprint_map_;
  std::array<FingerprintNeighbour, FingerprintMap::kMaxK> neighbours_{};  ///< Of the last scan that had any.
  int neighbour_count_ = 0;
  double last_fingerprint_t_ = -1e300;
  bool fingerprint_active_ = false;  ///< The update in progress applies `neighbours_`.
  double placeable_total_ = 0.0;     ///< Area a particle may be placed in, all levels together [m^2].
  std::vector<double> level_weight_;
  LevelSelector level_selector_;  ///< The reported level; the filter's own decisions use the most likely one.
};

}  // namespace ipe
