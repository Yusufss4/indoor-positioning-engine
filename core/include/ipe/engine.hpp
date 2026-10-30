#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/baro.hpp"
#include "ipe/ekf.hpp"
#include "ipe/filter.hpp"
#include "ipe/particle_filter.hpp"
#include "ipe/pdr.hpp"
#include "ipe/rooms.hpp"
#include "ipe/rssi_model.hpp"
#include "ipe/venue.hpp"
#include "ipe/wcl.hpp"

namespace ipe {

enum class FilterKind { ParticleFilter, Ekf, WeightedCentroid };

std::string_view to_string(FilterKind kind);
/// Accepts "pf", "ekf" and "wcl". Throws std::invalid_argument otherwise.
FilterKind parse_filter_kind(std::string_view name);

struct EngineConfig {
  FilterKind filter = FilterKind::ParticleFilter;
  PdrConfig pdr;
  RssiModelConfig rssi;
  BaroConfig baro;
  ParticleFilterConfig pf;
  EkfConfig ekf;
  WclConfig wcl;
  RoomEventConfig rooms;
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

  [[nodiscard]] Estimate estimate() const { return filter_->estimate(); }
  [[nodiscard]] const Filter& filter() const { return *filter_; }
  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] const EngineConfig& config() const { return cfg_; }
  [[nodiscard]] std::uint64_t step_count() const { return steps_; }

  /// Null unless the active filter is the particle filter.
  [[nodiscard]] const ParticleFilter* particle_filter() const { return pf_; }

  /// Room Enter/Exit events since the last call, oldest first. Always empty
  /// for venues without rooms.
  [[nodiscard]] std::vector<RoomEvent> take_room_events();
  /// Indices into Venue::rooms() of the rooms the device is currently in.
  [[nodiscard]] std::vector<std::size_t> current_rooms() const;
  /// Null for venues without rooms.
  [[nodiscard]] const RoomTracker* room_tracker() const { return rooms_ ? &*rooms_ : nullptr; }

 private:
  void update_rooms(double t) {
    if (rooms_ && rooms_->due(t)) rooms_->update(t, filter_->estimate(), pf_);
  }

  const Venue* venue_;
  EngineConfig cfg_;
  PdrProcessor pdr_;
  std::unique_ptr<Filter> filter_;
  const ParticleFilter* pf_ = nullptr;
  std::optional<RoomTracker> rooms_;
  std::uint64_t steps_ = 0;
};

std::unique_ptr<Filter> make_filter(const Venue& venue, const EngineConfig& cfg);

}  // namespace ipe
