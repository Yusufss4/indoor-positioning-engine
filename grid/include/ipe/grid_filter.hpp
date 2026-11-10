#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/beacon_integrity.hpp"
#include "ipe/filter.hpp"
#include "ipe/floor_classifier.hpp"
#include "ipe/level_selector.hpp"
#include "ipe/map_estimate.hpp"
#include "ipe/particle.hpp"
#include "ipe/pdr.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// Settings of the grid filter (a prototype next to the particle filter, see docs/grid.md). Where a field
/// has a particle filter counterpart, the default is the particle filter's.
struct GridFilterConfig {
  // The lattice (GridMap). Changing these builds another map.
  double cell_m = 0.5;                  ///< Cell size [m]; coarser when the walkable area needs more than max_cells.
  double max_cells = 200000;            ///< Walkable cells over all levels, at most.
  double max_reach_m = 2.0;             ///< Longest move of one step from a cell centre (transition mask radius).
  double closed_region_fraction = 0.1;  ///< Rooms without a door are not cells (as pf.closed_region_*).
  double closed_region_min_m2 = 10.0;

  int heading_bins = 36;  ///< Bins of the heading offset from the gyro-integrated yaw.
  double heading_sigma_deg = 3.0;
  double heading_outlier_prob = 0.15;

  double step_length_sigma = 0.08;  ///< Along-track noise per step [m].
  /// Along-track noise per step, relative to the step length. It also stands in for the step-length scale
  /// the particle filter carries per particle (experiment 14: 0.2 beat 0.1 and 0.15, and a scale state).
  double step_length_rel_sigma = 0.2;
  /// A persistent step-length scale as a state with this many values (1: none, odd).
  int step_scale_bins = 1;
  double step_scale_spread = 0.15;   ///< Spacing of the scale values.
  double step_scale_sigma = 0.15;    ///< Prior spread of the scale.
  double step_scale_switch = 0.002;  ///< Per step, probability of moving to a neighbouring scale value.

  bool estimate_rssi_offset = true;  ///< Device RSSI offset, a Gaussian per cell.
  double rssi_offset_sigma = 8.0;
  double rssi_offset_walk = 0.05;  ///< Random walk per RSSI update [dB].

  /// Recovery: uniform mass mixed in at this rate (a walker "kidnapped" anywhere), at most every
  /// teleport_period_s. Kept tiny: the mass is everywhere at once, so a misbehaving beacon or one Wi-Fi scan
  /// finds a foothold in it wherever the map allows one (experiment 14: 1e-3 to 1e-4 cost the moved-beacon
  /// walk and the IPIN buildings; 1e-8 keeps a way back from a wrong lock at no measurable cost).
  double teleport_rate_per_s = 1e-8;
  double teleport_period_s = 0.5;
  /// Adaptive recovery on top (augmented MCL's trigger): each teleport also mixes in 1 - w_fast / w_slow of
  /// uniform mass, at most recovery_max_fraction, where w_* are averages of the radio updates' predictive
  /// likelihood. Off when recovery_max_fraction is 0.
  double recovery_alpha_slow = 0.02;
  double recovery_alpha_fast = 0.05;
  double recovery_max_fraction = 0.0;
  /// Cells holding less than this share of the heaviest cell's mass are background: recovery mass, which
  /// is spread evenly, so steps leave it in place and it is uniform over headings. Radio and barometer
  /// still update it. 0 treats every cell in full (exact, and slow on large venues).
  double active_floor_rel = 0.0;

  double level_change_prob_step = 0.08;
  double level_change_prob_baro = 0.1;
  double baro_update_interval_s = 0.5;
  double baro_sigma_m = 0.8;
  double baro_outlier_prob = 0.05;
  double baro_offset_sigma_m = 0.3;
  double baro_offset_walk_m = 0.05;

  double level_smoothing_s = 0.5;
  double level_hysteresis = 0.1;
  bool map_consistent_estimate = true;

  bool beacon_integrity = true;
  double integrity_threshold_db = 10.0;
  double integrity_alpha = 0.1;
  int integrity_min_samples = 8;
  double integrity_max_sigma_m = 3.0;

  /// Floor prior from the k-NN classifier over the venue's Wi-Fi floor fingerprints, applied per scan as
  /// `floor_prior_weight * log p(level)` on every cell, as pf.floor_prior_weight does per particle. The radio
  /// model's 2-4 dB per slab barely separates floors, and the barometer only reports changes, so without it
  /// nothing rejects mass that has walked the same path one floor up. 0 (or a venue without fingerprints)
  /// turns it off. Negative: GridEngine takes pf.floor_prior_weight, _k and _eps from the engine config, so
  /// `--config ipin.json` turns it on for both filters; a GridFilter on its own treats it as 0.
  double floor_prior_weight = -1.0;
  int floor_prior_k = 5;
  double floor_prior_eps = 0.1;
};

/// The venue as a lattice of cells, built once and shared read-only by every GridFilter on that venue.
/// A cell is a lattice square whose centre is walkable on its level (inside the venue and the level's
/// footprint, outside closed regions). Moves between cells are allowed when the segment between their
/// centres crosses no wall: one bit per lattice offset within the reach, so a step's wall test is O(1).
/// Wall counts per (beacon, cell) for the radio model are computed the first time a beacon is used.
class GridMap {
 public:
  static constexpr int kMaxRadius = 5;  ///< Offsets per side; the mask holds (2R + 1)^2 bits.

  struct Cell {
    std::int32_t ix = 0;
    std::int32_t iy = 0;
    std::int32_t level = 0;
    std::int32_t up = -1;    ///< Cell above in a connector that serves both levels, else -1.
    std::int32_t down = -1;  ///< Cell below, likewise.
    std::array<std::uint64_t, 2> mask{};
  };

  GridMap(const Venue& venue, const GridFilterConfig& cfg);

  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] double cell_m() const { return cell_m_; }
  [[nodiscard]] int radius() const { return radius_; }
  [[nodiscard]] std::size_t size() const { return cells_.size(); }
  [[nodiscard]] const Cell& cell(std::size_t i) const { return cells_[i]; }
  [[nodiscard]] Vec2 centre(std::size_t i) const { return lattice_centre(cells_[i].ix, cells_[i].iy); }
  [[nodiscard]] Vec2 lattice_centre(int ix, int iy) const {
    return {origin_.x + (ix + 0.5) * cell_m_, origin_.y + (iy + 0.5) * cell_m_};
  }
  /// Cell index at lattice (ix, iy) on `level`, or -1.
  [[nodiscard]] std::int32_t at(int level, int ix, int iy) const {
    if (ix < 0 || iy < 0 || ix >= nx_ || iy >= ny_) return -1;
    return index_[(static_cast<std::size_t>(level) * static_cast<std::size_t>(ny_) + static_cast<std::size_t>(iy)) *
                      static_cast<std::size_t>(nx_) +
                  static_cast<std::size_t>(ix)];
  }
  /// Lattice column / row holding x / y (may be off the lattice).
  [[nodiscard]] int lattice_x(double x) const { return static_cast<int>(std::floor((x - origin_.x) / cell_m_)); }
  [[nodiscard]] int lattice_y(double y) const { return static_cast<int>(std::floor((y - origin_.y) / cell_m_)); }
  /// Lattice columns.
  [[nodiscard]] int lattice_nx() const { return nx_; }
  /// Cell index per lattice point of `level`, row by row (lattice_nx() per row), -1 where there is no cell.
  [[nodiscard]] const std::int32_t* lattice_index(int level) const {
    return index_.data() +
           static_cast<std::size_t>(level) * static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
  }
  /// Cell containing `p` on `level`, or -1.
  [[nodiscard]] std::int32_t locate(Vec2 p, int level) const { return at(level, lattice_x(p.x), lattice_y(p.y)); }
  /// Bit index of lattice offset (dx, dy), both within radius().
  [[nodiscard]] int bit(int dx, int dy) const { return (dy + radius_) * (2 * radius_ + 1) + dx + radius_; }
  [[nodiscard]] static bool test(const Cell& c, int bit) {
    return (c.mask[static_cast<std::size_t>(bit >> 6)] >> (bit & 63)) & 1u;
  }
  /// Cells on `level`, in index order: [level_begin(level), level_begin(level + 1)).
  [[nodiscard]] std::size_t level_begin(int level) const { return level_begin_[static_cast<std::size_t>(level)]; }

  /// Rooms (indices into Venue::rooms()) containing cell i's centre.
  [[nodiscard]] std::span<const std::uint32_t> rooms(std::size_t i) const {
    return {room_list_.data() + room_begin_[i], room_begin_[i + 1] - room_begin_[i]};
  }
  /// Cells with a connector link (up or down), for the level-change loop.
  [[nodiscard]] std::span<const std::uint32_t> connector_cells() const { return connector_cells_; }

  /// Walls on the cell's level between its centre and the beacon (Venue::count_wall_crossings), per cell.
  /// Thread-safe; computed the first time the beacon is used.
  [[nodiscard]] std::span<const std::uint8_t> wall_counts(std::size_t beacon) const;

 private:
  [[nodiscard]] std::vector<std::uint8_t> exact_wall_counts(std::size_t beacon) const;

  const Venue* venue_;
  double cell_m_ = 0.5;
  Vec2 origin_;
  int nx_ = 1;
  int ny_ = 1;
  int radius_ = 4;
  std::vector<std::int32_t> index_;  ///< Per level and lattice point: cell index or -1.
  std::vector<Cell> cells_;
  std::vector<std::size_t> level_begin_;
  std::vector<std::uint32_t> room_begin_;
  std::vector<std::uint32_t> room_list_;
  std::vector<std::uint32_t> connector_cells_;
  mutable std::vector<std::vector<std::uint8_t>> walls_;
  mutable std::unique_ptr<std::once_flag[]> walls_once_;
};

/// Bayes filter on the GridMap: probability per cell, with the heading (an offset from the gyro-integrated
/// yaw) and optionally a step scale as discrete states inside each cell, and the device RSSI offset and the
/// barometer offset as a Gaussian per cell. Deterministic: no random numbers. O(cells x headings) per step
/// and magnetometer update, O(cells x readings) per radio update; nothing is allocated after construction
/// except the wall counts of a beacon the first time it is heard (in the shared GridMap).
class GridFilter {
 public:
  GridFilter(std::shared_ptr<const GridMap> map, GridFilterConfig cfg, RssiModelConfig rssi_cfg,
             BaroConfig baro_cfg = {});

  void on_step(const StepEvent& step);
  void on_heading(double t, double heading, double sigma);
  void on_rssi(double t, std::size_t beacon_index, double rssi);
  void on_rssi_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);

  [[nodiscard]] Estimate estimate() const { return estimate_; }
  /// The step's own move: mean displacement of the mass that survived it (see ParticleFilter::step_motion).
  [[nodiscard]] Vec2 step_motion() const { return step_motion_; }

  [[nodiscard]] const GridMap& map() const { return *map_; }
  [[nodiscard]] const GridFilterConfig& config() const { return cfg_; }
  /// Probability of each cell (sums to 1).
  [[nodiscard]] std::span<const double> mass() const { return mass_; }
  /// Mass centroid of each cell.
  [[nodiscard]] Vec2 centroid(std::size_t cell) const { return {cx_[cell], cy_[cell]}; }
  /// Heading distribution of a cell over heading_bins (step scales summed).
  [[nodiscard]] double heading_prob(std::size_t cell, int bin) const;
  /// Absolute heading of a bin [rad].
  [[nodiscard]] double bin_heading(int bin) const;
  [[nodiscard]] double rssi_offset_mean(std::size_t cell) const { return dev_mu_[cell]; }
  [[nodiscard]] double rssi_offset_var(std::size_t cell) const { return dev_var_[cell]; }
  [[nodiscard]] bool beacon_excluded(std::size_t beacon_index) const { return integrity_.excluded(beacon_index); }

  /// Replaces the distribution by a point mass at `p` on `level` with heading bin `bin` (all scales), for
  /// tests.
  void set_point(Vec2 p, int level, int bin);

 private:
  void initialize_uniform();
  bool check_beacon(std::size_t beacon_index, double rssi);
  void update_rssi(double t, std::span<const RssiReading> readings);
  /// Mixes uniform mass in if the teleport period has passed.
  void maybe_teleport(double t);
  /// Moves `prob` of the mass of each connector cell to the levels next to it.
  void change_levels(double prob);
  /// Multiplies masses by exp(loglik - max) and renormalises; the estimate follows.
  void apply_cell_loglik(double t);
  void normalize();
  /// Mass below which a cell is background (see active_floor_rel).
  [[nodiscard]] double active_floor() const;
  void update_estimate(double t);
  /// Recomputes the cached heading of the cloud on `level` (H changes only at steps and headings).
  void update_heading(int level);
  [[nodiscard]] std::size_t state(std::size_t cell, int scale, int bin) const {
    return (cell * static_cast<std::size_t>(scales_) + static_cast<std::size_t>(scale)) *
               static_cast<std::size_t>(bins_) +
           static_cast<std::size_t>(bin);
  }
  [[nodiscard]] double expected_rssi(std::size_t beacon, std::size_t cell, Vec2 pos) const;
  [[nodiscard]] double elevation(int level) const { return map_->venue().level(level).elevation_m; }

  std::shared_ptr<const GridMap> map_;
  GridFilterConfig cfg_;
  RssiModel rssi_model_;
  int bins_ = 36;
  int scales_ = 1;
  bool multi_level_ = false;
  std::vector<double> scale_value_;
  std::vector<double> scale_prior_;

  // Per cell.
  std::vector<double> mass_;
  std::vector<double> cx_, cy_;
  std::vector<double> dev_mu_, dev_var_;
  std::vector<double> baro_mu_, baro_var_;
  std::vector<double> loglik_;
  // Per (cell, scale, heading): conditional distribution within the cell.
  std::vector<float> h_;
  // Scratch for the step.
  std::vector<float> h_next_;
  std::vector<std::uint8_t> touched_;  ///< Cells some active mass moved into during the current step.
  /// What a step moves into a cell: mass, mass times centroid, and mass times the offsets' first and second
  /// moments, side by side (64 bytes per cell) rather than in seven arrays.
  struct StepAcc {
    double mass, x, y, d1, d2, b1, b2, unused;
  };
  std::vector<StepAcc> acc_;
  std::vector<double> move_x_, move_y_;     ///< Per (scale, heading, node): the move of the current step [m].
  std::vector<int> move_offset_;            ///< The same move's lattice offset from the cell being moved, or -1.
  std::vector<std::int32_t> local_target_;  ///< Per lattice offset: target cell of the cell being moved, or -1.
  std::vector<double> local_w_, local_x_, local_y_;
  std::vector<double> row_lik_;  ///< Magnetometer likelihood per (scale, heading).
  std::vector<double> bin_lik_;  ///< Per heading bin: magnetometer likelihood, or the cloud's heading histogram.
  std::vector<double> row_;      ///< One cell's (scale, heading) row in double precision.
  std::vector<RssiReading> scan_;
  std::vector<double> level_weight_;
  std::vector<Particle> points_;  ///< Cells as particles for MapConsistentEstimate.
  // Scratch for level changes: per connector cell, a copy of its state before the change.
  std::vector<double> conn_mass_, conn_x_, conn_y_, conn_d1_, conn_d2_, conn_b1_, conn_b2_;
  std::vector<float> conn_h_;

  BeaconIntegrityMonitor integrity_;
  FloorClassifier floor_classifier_;
  std::vector<double> floor_p_;      ///< Level probabilities of the scan being applied.
  bool floor_prior_active_ = false;  ///< Set while update_rssi applies a classified scan.
  LevelSelector level_selector_;
  std::optional<MapConsistentEstimate> map_estimate_;
  BaroAltimeter altimeter_;
  double yaw_ = 0.0;
  double now_ = 0.0;
  double last_teleport_t_ = 0.0;
  bool teleport_started_ = false;
  double w_slow_ = 0.0;
  double w_fast_ = 0.0;
  bool localized_ = false;
  bool baro_ready_ = false;
  double last_baro_update_t_ = -1e300;
  double altitude_ = 0.0;
  double heading_ = 0.0;  ///< Cached cloud heading.
  double mean_rssi_offset_ = 0.0;
  Estimate estimate_;
  Estimate cloud_;
  Vec2 step_motion_;
};

}  // namespace ipe
