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

#include "ipe/engine.hpp"
#include "ipe/grid_filter.hpp"
#include "ipe/rooms.hpp"
#include "ipe/track_smoother.hpp"

namespace ipe {

/// PositioningEngine's pipeline with the grid filter in place of the particle filter: PDR steps, the
/// magnetometer heading after each step, radio, pressure, the same TrackSmoother and room events from the
/// mass in each room's cells. The particle filter settings (`base.pf`) are not used.
class GridEngine {
 public:
  GridEngine(const Venue& venue, std::shared_ptr<const GridMap> map, const EngineConfig& base,
             const GridFilterConfig& grid);

  void on_imu(const ImuSample& sample);
  void on_ble(double t, std::size_t beacon_index, double rssi);
  void on_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);

  /// What to draw: the filter's estimate through the track smoother.
  [[nodiscard]] Estimate estimate() const {
    return cfg_.smoothing.enabled ? smoother_.estimate(now_, filter_.estimate()) : filter_.estimate();
  }
  [[nodiscard]] Estimate raw_estimate() const { return filter_.estimate(); }
  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] const GridFilter& filter() const { return filter_; }
  [[nodiscard]] std::uint64_t step_count() const { return steps_; }
  [[nodiscard]] const RoomTracker* room_tracker() const { return rooms_ ? &*rooms_ : nullptr; }
  [[nodiscard]] std::vector<RoomEvent> take_room_events() {
    return rooms_ ? rooms_->take_events() : std::vector<RoomEvent>{};
  }

 private:
  void after_update(double t);
  void update_rooms(double t);

  const Venue* venue_;
  EngineConfig cfg_;
  PdrProcessor pdr_;
  GridFilter filter_;
  TrackSmoother smoother_;
  double now_ = 0.0;
  std::optional<RoomTracker> rooms_;
  std::vector<double> room_prob_;
  std::uint64_t steps_ = 0;
};

/// Up to `max` of the heaviest cells as (centroid x, y, mass, level), for the particle snapshots of a replay.
void append_snapshot(const GridEngine& engine, std::vector<std::array<float, 4>>& out, std::size_t max);

namespace grid {

/// RFC 7386 merge patch on top of `cfg`; unknown keys throw.
void apply_overrides(GridFilterConfig& cfg, std::string_view json_patch);
std::string config_to_json(const GridFilterConfig& cfg, int indent = 2);

}  // namespace grid

}  // namespace ipe
