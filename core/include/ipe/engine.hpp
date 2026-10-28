#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "ipe/ekf.hpp"
#include "ipe/filter.hpp"
#include "ipe/particle_filter.hpp"
#include "ipe/pdr.hpp"
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
  ParticleFilterConfig pf;
  EkfConfig ekf;
  WclConfig wcl;
  double mag_heading_sigma_deg = 12.0;
};

/// Platform-independent entry point: feed raw IMU samples and BLE
/// advertisements in timestamp order, read back the fused estimate. This is
/// the surface a mobile SDK or the WebAssembly module would wrap.
class PositioningEngine {
 public:
  PositioningEngine(const Venue& venue, EngineConfig cfg);

  void on_imu(const ImuSample& sample);
  void on_ble(double t, std::string_view beacon_id, double rssi);
  void on_ble(double t, std::size_t beacon_index, double rssi);

  [[nodiscard]] Estimate estimate() const { return filter_->estimate(); }
  [[nodiscard]] const Filter& filter() const { return *filter_; }
  [[nodiscard]] const Venue& venue() const { return *venue_; }
  [[nodiscard]] const EngineConfig& config() const { return cfg_; }
  [[nodiscard]] std::uint64_t step_count() const { return steps_; }

  /// Null unless the active filter is the particle filter.
  [[nodiscard]] const ParticleFilter* particle_filter() const;

 private:
  const Venue* venue_;
  EngineConfig cfg_;
  PdrProcessor pdr_;
  std::unique_ptr<Filter> filter_;
  std::uint64_t steps_ = 0;
};

std::unique_ptr<Filter> make_filter(const Venue& venue, const EngineConfig& cfg);

}  // namespace ipe
