#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
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
  /// sample (see truth()). Must not be called when finished(). `Engine` is
  /// PositioningEngine, or anything with its on_imu/on_ble/on_scan/on_pressure.
  template <class Engine>
  Event step(Engine& engine);

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

template <class Engine>
TracePlayer::Event TracePlayer::step(Engine& engine) {
  if (finished()) throw std::logic_error("TracePlayer::step after the end of the trace");
  const auto& tr = *trace_;
  const double ti = i_ < tr.imu.size() ? tr.imu[i_].t : kNever;
  const double tb = b_ < tr.ble.size() ? tr.ble[b_].t : kNever;
  const double tp = p_ < tr.baro.size() ? tr.baro[p_].t : kNever;
  const double tg = tr.truth[g_].t;

  if (ti <= tb && ti <= tp && ti <= tg) {
    engine.on_imu(tr.imu[i_++]);
    return Event::Imu;
  }
  if (tb <= tp && tb <= tg) {
    const std::size_t end = scan_end(tr.ble, b_);
    const std::size_t known = venue_->beacons().size();
    fed_ble_.clear();
    if (tr.ble[b_].scan != 0) {
      scan_.clear();
      for (std::size_t k = b_; k < end; ++k) {
        scan_.push_back({ble_index_[k], tr.ble[k].rssi});
        if (ble_index_[k] < known) fed_ble_.push_back(k);
      }
      engine.on_scan(tr.ble[b_].t, scan_);
    } else {
      engine.on_ble(tr.ble[b_].t, ble_index_[b_], tr.ble[b_].rssi);
      if (ble_index_[b_] < known) fed_ble_.push_back(b_);
    }
    b_ = end;
    return Event::Radio;
  }
  if (tp <= tg) {
    engine.on_pressure(tr.baro[p_].t, tr.baro[p_].hpa);
    ++p_;
    return Event::Pressure;
  }
  ++g_;
  return Event::Truth;
}

}  // namespace ipe
