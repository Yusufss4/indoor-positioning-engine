#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/estimate.hpp"
#include "ipe/grid_filter.hpp"
#include "ipe/pdr.hpp"
#include "ipe/rooms.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/track_smoother.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// The engine's settings: what a deployment changes. Everything else about the pipeline is fixed, as the
/// parameters and constants of its components. As JSON: ipe/config.hpp.
struct EngineConfig {
  struct Pdr {
    /// Scale of the step length (Weinberg's K): a walker whose steps are shorter than estimated needs less.
    double weinberg_k = PdrConfig{}.weinberg_k;
  };
  struct Rssi {
    /// Spread of RSSI around the path-loss curve [dB]: 5 for surveyed beacons, 8 for a fitted radio map.
    double sigma_db = RssiModelConfig{}.sigma_db;
    /// Subtract the venue's loss per wall between beacon and phone.
    bool wall_aware = RssiModelConfig{}.wall_aware;
  };
  struct Filter {
    /// Side of a cell of the lattice [m].
    double cell_m = GridFilterConfig{}.cell_m;
    /// Update only where the mass is (GridFilterConfig::active_floor). 0: every cell at every update.
    double active_floor = GridFilterConfig{}.active_floor;
    /// Estimate the phone's RSSI offset against the radio map.
    bool estimate_rssi_offset = GridFilterConfig{}.estimate_rssi_offset;
    /// Distrust beacons whose readings keep disagreeing with the estimate (moved or dead beacons).
    bool beacon_integrity = GridFilterConfig{}.beacon_integrity;
    /// Weight of the floor vote of the venue's training scans. 0: off.
    double floor_prior_weight = GridFilterConfig{}.floor_prior_weight;
    /// Weight of the venue's training scans as position fingerprints. 0: off.
    double fingerprint_weight = GridFilterConfig{}.fingerprint_weight;
  };
  struct Smoothing {
    /// False: estimate() is the filter's estimate, bit for bit.
    bool enabled = TrackSmootherConfig{}.enabled;
    /// Time constant of the glide towards a correction [s].
    double tau_s = TrackSmootherConfig{}.tau_s;
    /// Cap on the speed of that glide [m/s]. 0: no cap.
    double max_correction_mps = TrackSmootherConfig{}.max_correction_mps;
    /// The drawn point jumps when the estimate is further away than this [m].
    double snap_m = TrackSmootherConfig{}.snap_m;
  };
  struct Rooms {
    double enter_prob = RoomEventConfig{}.enter_prob;  ///< Room probability that starts the enter dwell.
    double exit_prob = RoomEventConfig{}.exit_prob;    ///< Room probability that starts the exit dwell.
  };

  Pdr pdr;
  Rssi rssi;
  Filter filter;
  Smoothing smoothing;
  Rooms rooms;
};

/// The grid filter's parameters for these settings: what a GridMap shared between engines is built from.
[[nodiscard]] GridFilterConfig filter_params(const EngineConfig& cfg);

/// The config to run `venue` with, chosen from what the venue contains:
///  - surveyed beacons only: `EngineConfig{}`;
///  - training scans (`floor_fingerprints`): the settings for a radio map fitted from scans, with the floor
///    vote (data/configs/ipin.json);
///  - training scans with positions: the same plus the fingerprint likelihood (data/configs/fingerprint.json).
/// A Wi-Fi venue without training scans is not recognisable from the venue: use data/configs/msil_wifi.json.
[[nodiscard]] EngineConfig recommended_config(const Venue& venue);

/// Platform-independent entry point: feed raw IMU samples, BLE advertisements, Wi-Fi scans and
/// (optionally) barometer readings in timestamp order, read back the fused estimate. This is the surface
/// a mobile SDK or the WebAssembly module wraps.
///
/// The pipeline: PDR turns IMU samples into steps; the grid filter (GridFilter) holds the posterior over
/// the venue's cells and applies steps, the magnetometer heading after each step, radio and pressure; the
/// track smoother turns its estimate into what is drawn; room events come from the mass in each room's
/// cells.
class PositioningEngine {
 public:
  /// With recommended_config(venue): what to use unless a deployment needs its own settings.
  explicit PositioningEngine(const Venue& venue);
  /// Builds the venue's cell lattice (GridMap) from `filter_params(cfg)`.
  PositioningEngine(const Venue& venue, EngineConfig cfg);
  /// With a lattice built before, to share one between engines on the same venue: the lattice and its
  /// wall counts are the expensive part of start-up on a large venue. `map` must be of `venue` and of
  /// `filter_params(cfg)`.
  PositioningEngine(const Venue& venue, EngineConfig cfg, std::shared_ptr<const GridMap> map);

  void on_imu(const ImuSample& sample);
  /// A step from a motion model of your own, in place of the built-in step detector: feed these and no IMU
  /// samples. `step.heading_delta` is the turn since the previous step; a valid `step.mag_heading` is
  /// applied as an absolute heading with the magnetometer's spread (12 degrees).
  void on_step(const StepEvent& step);
  /// An absolute heading [rad] with its standard deviation [rad], from any source.
  void on_heading(double t, double heading, double sigma);
  void on_ble(double t, std::string_view beacon_id, double rssi);
  void on_ble(double t, std::size_t beacon_index, double rssi);
  /// One scan's readings; beacon indices outside the venue are dropped.
  void on_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);
  /// A position known from outside the engine: a tap on the map, a QR code or NFC tag at a known place, a
  /// GNSS fix at an entrance. `level` is a level index, or negative when the floor is not known; `sigma_m`
  /// is how well the position is known. Without radio it is what makes the estimate valid: steps and the
  /// map carry it on from there.
  void on_position_fix(double t, Vec2 pos, int level, double sigma_m);

  /// What to draw: the filter's estimate through the track smoother (EngineConfig::smoothing).
  [[nodiscard]] Estimate estimate() const {
    return cfg_.smoothing.enabled ? smoother_.estimate(now_, filter_.estimate()) : filter_.estimate();
  }
  /// The filter's own estimate, unsmoothed.
  [[nodiscard]] Estimate raw_estimate() const { return filter_.estimate(); }
  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] const EngineConfig& config() const { return cfg_; }
  [[nodiscard]] std::uint64_t step_count() const { return steps_; }

  [[nodiscard]] const GridFilter& filter() const { return filter_; }
  [[nodiscard]] const GridMap& map() const { return filter_.map(); }

  /// Room Enter/Exit events since the last call, oldest first. Always empty
  /// for venues without rooms.
  [[nodiscard]] std::vector<RoomEvent> take_room_events();
  /// Indices into Venue::rooms() of the rooms the device is currently in.
  [[nodiscard]] std::vector<std::size_t> current_rooms() const;
  /// Null for venues without rooms.
  [[nodiscard]] const RoomTracker* room_tracker() const { return rooms_ ? &*rooms_ : nullptr; }

 private:
  void apply_step(const StepEvent& step);
  /// Feeds the filter's estimate after a non-step update to the smoother.
  void after_update(double t);
  void update_rooms(double t);

  const Venue* venue_;
  EngineConfig cfg_;
  PdrProcessor pdr_;
  GridFilter filter_;
  TrackSmoother smoother_;
  double now_ = 0.0;  ///< Latest event time.
  std::optional<RoomTracker> rooms_;
  std::vector<double> room_prob_;
  std::uint64_t steps_ = 0;
};

/// Up to `max` of the heaviest cells as (centroid x, y, mass, level): what a replay records as a snapshot
/// of the posterior.
void append_snapshot(const PositioningEngine& engine, std::vector<std::array<float, 4>>& out, std::size_t max);

}  // namespace ipe
