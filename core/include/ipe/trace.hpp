#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/pdr.hpp"

namespace ipe {

struct BleSample {
  double t = 0.0;
  std::string beacon_id;
  double rssi = 0.0;
  /// Non-zero for readings delivered together in one scan (Wi-Fi). Readings of
  /// one scan share `t` and are contiguous in the stream; 0 is a lone packet.
  std::uint32_t scan = 0;
};

struct BaroSample {
  double t = 0.0;
  double hpa = 0.0;  ///< Barometric pressure as reported by the phone.
};

struct TruthSample {
  double t = 0.0;
  Vec2 pos;
  double heading = 0.0;
  int level = 0;   ///< Level index in the venue (nearest floor while on stairs or in a lift).
  double z = 0.0;  ///< Height of the walker's floor surface above the venue datum [m].
};

/// A recorded or simulated session. Each stream is sorted by time.
struct Trace {
  std::map<std::string, std::string> metadata;
  std::vector<ImuSample> imu;
  std::vector<BleSample> ble;
  std::vector<BaroSample> baro;  ///< Optional; empty when the phone has no barometer.
  std::vector<TruthSample> truth;

  [[nodiscard]] double duration() const;
};

/// One past the last sample of the scan that starts at `ble[begin]`; `begin + 1`
/// for a lone packet.
std::size_t scan_end(const std::vector<BleSample>& ble, std::size_t begin);

/// Writes the line-oriented CSV format described in docs/formats.md.
void write_trace(std::ostream& out, const Trace& trace);
void write_trace_file(const std::string& path, const Trace& trace);

Trace read_trace(std::istream& in);
Trace read_trace_file(const std::string& path);

}  // namespace ipe
