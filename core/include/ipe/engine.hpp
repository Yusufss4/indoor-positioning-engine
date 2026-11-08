#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/filter.hpp"
#include "ipe/particle_filter.hpp"
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
  ParticleFilterConfig pf;
  RoomEventConfig rooms;
  TrackSmootherConfig smoothing;
  double mag_heading_sigma_deg = 12.0;
};

/// Platform-independent entry point: feed raw IMU samples, BLE advertisements
/// and (optionally) barometer readings in timestamp order, read back the fused
/// estimate. This is the surface a mobile SDK or the WebAssembly module would wrap.
class PositioningEngine {
 public:
  PositioningEngine(const Venue& venue, EngineConfig cfg);

  void on_imu(const ImuSample& sample);
  void on_ble(double t, std::string_view beacon_id, double rssi);
  void on_ble(double t, std::size_t beacon_index, double rssi);
  /// One scan's readings; beacon indices outside the venue are dropped.
  void on_scan(double t, std::span<const RssiReading> readings);
  void on_pressure(double t, double hpa);

  /// What to draw: the filter's estimate through the track smoother (EngineConfig::smoothing).
  [[nodiscard]] Estimate estimate() const {
    return cfg_.smoothing.enabled ? smoother_.estimate(now_, pf_.estimate()) : pf_.estimate();
  }
  /// The particle filter's own estimate, unsmoothed.
  [[nodiscard]] Estimate raw_estimate() const { return pf_.estimate(); }
  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] const EngineConfig& config() const { return cfg_; }
  [[nodiscard]] std::uint64_t step_count() const { return steps_; }

  [[nodiscard]] const ParticleFilter& particle_filter() const { return pf_; }

  /// Room Enter/Exit events since the last call, oldest first. Always empty
  /// for venues without rooms.
  [[nodiscard]] std::vector<RoomEvent> take_room_events();
  /// Indices into Venue::rooms() of the rooms the device is currently in.
  [[nodiscard]] std::vector<std::size_t> current_rooms() const;
  /// Null for venues without rooms.
  [[nodiscard]] const RoomTracker* room_tracker() const { return rooms_ ? &*rooms_ : nullptr; }

 private:
  /// Feeds the filter's estimate after a non-step update to the smoother.
  void after_update(double t) {
    now_ = std::max(now_, t);
    smoother_.on_update(t, pf_.estimate());
    update_rooms(t);
  }
  void update_rooms(double t) {
    if (rooms_ && rooms_->due(t)) rooms_->update(t, pf_.estimate(), &pf_);
  }

  const Venue* venue_;
  EngineConfig cfg_;
  PdrProcessor pdr_;
  ParticleFilter pf_;
  TrackSmoother smoother_;
  double now_ = 0.0;  ///< Latest event time.
  std::optional<RoomTracker> rooms_;
  std::uint64_t steps_ = 0;
};

}  // namespace ipe
