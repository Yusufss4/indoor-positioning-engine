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

struct EngineConfig {
  PdrConfig pdr;
  RssiModelConfig rssi;
  BaroConfig baro;
  GridFilterConfig filter;
  RoomEventConfig rooms;
  TrackSmootherConfig smoothing;
  double mag_heading_sigma_deg = 12.0;
};

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
  /// Builds the venue's cell lattice (GridMap) from `cfg.filter`.
  PositioningEngine(const Venue& venue, EngineConfig cfg);
  /// With a lattice built before, to share one between engines on the same venue: the lattice and its
  /// wall counts are the expensive part of start-up on a large venue. `map` must be of `venue` and of the
  /// lattice settings in `cfg.filter`.
  PositioningEngine(const Venue& venue, EngineConfig cfg, std::shared_ptr<const GridMap> map);

  void on_imu(const ImuSample& sample);
  /// A step from a motion model of your own, in place of the built-in step detector: feed these and no IMU
  /// samples. `step.heading_delta` is the turn since the previous step; a valid `step.mag_heading` is
  /// applied as an absolute heading with `mag_heading_sigma_deg`.
  void on_step(const StepEvent& step);
  /// An absolute heading [rad] with its standard deviation [rad], from any source.
  void on_heading(double t, double heading, double sigma);
  void on_ble(double t, std::string_view beacon_id, double rssi);
  void on_ble(double t, std::size_t beacon_index, double rssi);
  /// One scan's readings; beacon indices outside the venue are dropped.
  void on_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);

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
