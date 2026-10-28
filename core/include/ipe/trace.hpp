#pragma once

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
};

struct TruthSample {
  double t = 0.0;
  Vec2 pos;
  double heading = 0.0;
};

/// A recorded or simulated session. Each stream is sorted by time.
struct Trace {
  std::map<std::string, std::string> metadata;
  std::vector<ImuSample> imu;
  std::vector<BleSample> ble;
  std::vector<TruthSample> truth;

  [[nodiscard]] double duration() const;
};

/// Writes the line-oriented CSV format described in docs/formats.md.
void write_trace(std::ostream& out, const Trace& trace);
void write_trace_file(const std::string& path, const Trace& trace);

Trace read_trace(std::istream& in);
Trace read_trace_file(const std::string& path);

}  // namespace ipe
