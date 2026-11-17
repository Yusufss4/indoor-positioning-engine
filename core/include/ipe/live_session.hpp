#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/trace.hpp"
#include "ipe/venue.hpp"

namespace ipe {

/// The engine fed live, one sensor event at a time, with everything it was fed kept as a trace: what an app
/// on a phone holds (the browser's live page, the Android example).
///
/// Every value is rounded to the trace file's precision before it reaches the engine, and every event gets
/// a time in whole milliseconds strictly after the previous one. Replaying the saved trace (`ipe-replay`,
/// TracePlayer) therefore gives exactly the estimates the session gave.
///
/// Radio readings are recorded under the id the phone reports, whether or not the venue lists it; the
/// engine uses those it knows. A walk recorded on a venue without a radio map keeps them all, which is what
/// datasets/fit_radio_map.py fits one from.
class LiveSession {
 public:
  /// One reading of a scan.
  struct Reading {
    std::string_view id;
    double rssi = 0.0;  ///< dBm.
  };

  /// With the settings the venue calls for (recommended_config), then `config_patch`, a JSON merge patch
  /// that may be empty.
  explicit LiveSession(Venue venue, std::string_view config_patch = {});
  LiveSession(const LiveSession&) = delete;
  LiveSession& operator=(const LiveSession&) = delete;

  /// An inertial sample in the engine's frame (docs/formats.md).
  void on_imu(double t, Vec3 accel, Vec3 gyro, Vec3 mag);
  /// One packet of one transmitter.
  void on_ble(double t, std::string_view id, double rssi);
  /// The readings a scan delivered together (Wi-Fi), applied as one update. An empty scan is ignored.
  void on_scan(double t, std::span<const Reading> readings);
  void on_pressure(double t, double hpa);
  /// A position known from outside: the tap that says "I am here, start from this".
  void on_position_fix(double t, Vec2 pos, int level, double sigma_m);
  /// A place the user says they are at, kept as ground truth and not shown to the engine. Returns the
  /// estimate's distance from it, or -1 without a fix or on another floor.
  double mark(double t, Vec2 pos, int level);

  [[nodiscard]] Estimate estimate() const { return engine_.estimate(); }
  [[nodiscard]] const PositioningEngine& engine() const { return engine_; }
  [[nodiscard]] const Venue& venue() const { return venue_; }
  /// Time of the last event [s].
  [[nodiscard]] double time() const { return last_t_; }

  /// Adds a `# key: value` line to the saved trace.
  void set_metadata(const std::string& key, const std::string& value) { trace_.metadata[key] = value; }
  /// Everything fed so far. Replay scores it up to the last mark.
  [[nodiscard]] const Trace& trace() const { return trace_; }
  /// The same as a trace file (docs/formats.md).
  [[nodiscard]] std::string trace_csv() const;

 private:
  /// Event time in whole milliseconds, strictly after the previous event: a replay orders events by time
  /// alone, so two events of one millisecond would be fed in the trace player's order, not in this one.
  double stamp(double t);

  Venue venue_;  ///< Declared before the engine, which keeps a pointer to it.
  PositioningEngine engine_;
  Trace trace_;
  std::vector<RssiReading> scan_;
  std::uint32_t scans_ = 0;
  double last_ms_ = -1.0;
  double last_t_ = 0.0;
};

}  // namespace ipe
