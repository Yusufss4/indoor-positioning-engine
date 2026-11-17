#include "ipe/live_session.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

#include "ipe/config.hpp"

namespace ipe {

namespace {

/// Rounds to the precision the trace file keeps (`scale` = 10^decimals).
double q(double v, double scale) { return std::round(v * scale) / scale; }

EngineConfig config_for(const Venue& venue, std::string_view patch) {
  EngineConfig cfg = recommended_config(venue);
  apply_config_patch(cfg, patch);
  return cfg;
}

}  // namespace

LiveSession::LiveSession(Venue venue, std::string_view config_patch)
    : venue_(std::move(venue)), engine_(venue_, config_for(venue_, config_patch)) {
  trace_.metadata["venue"] = venue_.name();
}

double LiveSession::stamp(double t) {
  const double ms = std::max(std::round(t * 1000.0), last_ms_ + 1.0);
  last_ms_ = ms;
  last_t_ = ms / 1000.0;
  return last_t_;
}

void LiveSession::on_imu(double t, Vec3 accel, Vec3 gyro, Vec3 mag) {
  ImuSample s;
  s.t = stamp(t);
  s.accel = {q(accel.x, 1e4), q(accel.y, 1e4), q(accel.z, 1e4)};
  s.gyro = {q(gyro.x, 1e5), q(gyro.y, 1e5), q(gyro.z, 1e5)};
  s.mag = {q(mag.x, 1e3), q(mag.y, 1e3), q(mag.z, 1e3)};
  trace_.imu.push_back(s);
  engine_.on_imu(s);
}

void LiveSession::on_ble(double t, std::string_view id, double rssi) {
  trace_.ble.push_back({stamp(t), std::string(id), q(rssi, 1.0), 0});
  // By index, as a replay feeds it: an id the venue does not list is one past the last beacon.
  engine_.on_ble(trace_.ble.back().t, venue_.beacon_index(id).value_or(venue_.beacons().size()),
                 trace_.ble.back().rssi);
}

void LiveSession::on_scan(double t, std::span<const Reading> readings) {
  if (readings.empty()) return;
  const double ts = stamp(t);
  ++scans_;
  scan_.clear();
  for (const Reading& r : readings) {
    trace_.ble.push_back({ts, std::string(r.id), q(r.rssi, 1.0), scans_});
    scan_.push_back({venue_.beacon_index(r.id).value_or(venue_.beacons().size()), trace_.ble.back().rssi});
  }
  engine_.on_scan(ts, scan_);
}

void LiveSession::on_pressure(double t, double hpa) {
  trace_.baro.push_back({stamp(t), q(hpa, 1e4)});
  engine_.on_pressure(trace_.baro.back().t, trace_.baro.back().hpa);
}

void LiveSession::on_position_fix(double t, Vec2 pos, int level, double sigma_m) {
  const FixSample fix{stamp(t), {q(pos.x, 1e3), q(pos.y, 1e3)}, level, q(sigma_m, 1e3)};
  trace_.fixes.push_back(fix);
  engine_.on_position_fix(fix.t, fix.pos, fix.level, fix.sigma_m);
}

double LiveSession::mark(double t, Vec2 pos, int level) {
  TruthSample truth;
  truth.t = stamp(t);
  truth.pos = {q(pos.x, 1e3), q(pos.y, 1e3)};
  truth.level = std::clamp(level, 0, venue_.level_count() - 1);
  truth.z = q(venue_.level(truth.level).elevation_m, 1e3);
  trace_.truth.push_back(truth);
  const Estimate e = engine_.estimate();
  return e.valid && e.level == truth.level ? distance(e.pos, truth.pos) : -1.0;
}

std::string LiveSession::trace_csv() const {
  std::ostringstream out;
  write_trace(out, trace_);
  return out.str();
}

}  // namespace ipe
