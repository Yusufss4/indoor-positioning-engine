#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/trace.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// Feeds a recorded or simulated trace into a PositioningEngine in timestamp
/// order: the one place that knows how the trace's streams interleave. On equal
/// timestamps IMU comes first, then radio, then pressure, then the truth sample.
/// Readings that share a scan id are delivered as one scan; packets from
/// beacons the venue does not know reach the engine with an out-of-range index,
/// which it ignores. A new sensor stream is added here once, and every replay
/// (eval, the web demo) picks it up.
class TracePlayer {
 public:
  enum class Event { Imu, Radio, Pressure, Truth };

  /// Both must outlive the player. Beacon ids are resolved once, here.
  TracePlayer(const Venue& venue, const Trace& trace);

  /// True once every truth sample has been consumed; the replay is then over.
  [[nodiscard]] bool finished() const { return g_ >= trace_->truth.size(); }
  /// Timestamp of the next event; +infinity when finished.
  [[nodiscard]] double next_time() const;
  /// Feeds the next sensor event to `engine`, or consumes the next truth
  /// sample (see truth()). Must not be called when finished().
  Event step(PositioningEngine& engine);

  /// The truth sample consumed by the last Truth step.
  [[nodiscard]] const TruthSample& truth() const { return trace_->truth[g_ - 1]; }
  /// Indices into trace.ble of the known-beacon readings fed by the last Radio step.
  [[nodiscard]] std::span<const std::size_t> fed_ble() const { return fed_ble_; }

 private:
  static constexpr double kNever = std::numeric_limits<double>::infinity();

  const Venue* venue_;
  const Trace* trace_;
  std::vector<std::size_t> ble_index_;  ///< Venue beacon index per trace.ble row; beacons().size() if unknown.
  std::vector<RssiReading> scan_;
  std::vector<std::size_t> fed_ble_;
  std::size_t i_ = 0;  ///< Next imu sample.
  std::size_t b_ = 0;  ///< Next ble row.
  std::size_t p_ = 0;  ///< Next barometer sample.
  std::size_t g_ = 0;  ///< Next truth sample.
};

}  // namespace ipe
