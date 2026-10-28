#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/trace.hpp"
#include "ipe/venue.hpp"

namespace ipe::sim {

struct WalkConfig {
  std::vector<Vec2> waypoints;
  double step_frequency_hz = 1.8;
  double step_length_m = 0.7;
  double step_length_sigma = 0.04;
  double turn_rate_deg_s = 150.0;
  double waypoint_radius_m = 0.3;
};

/// A local magnetic anomaly (steel structure, elevator, etc.) adding a
/// horizontal world-frame field that fades linearly to zero at `radius`.
struct MagDisturbance {
  Vec2 center;
  double radius = 3.0;
  Vec2 field;  ///< microtesla
};

struct ImuConfig {
  double rate_hz = 50.0;
  double accel_noise = 0.25;   ///< m/s^2
  double gyro_noise = 0.02;    ///< rad/s
  double gyro_bias = 0.004;    ///< rad/s, constant for the session
  double mag_field_ut = 40.0;  ///< Horizontal Earth field magnitude
  double mag_noise_ut = 1.5;
  double weinberg_k = 0.48;  ///< The walker's true Weinberg constant.
  std::vector<MagDisturbance> mag_disturbances;
};

struct BleConfig {
  double rx_interval_s = 0.5;  ///< Mean interval between received packets per beacon.
  double rx_jitter = 0.3;      ///< Relative jitter of the interval.
  double noise_db = 4.5;       ///< Fast fading, i.i.d. per packet.
  double shadowing_db = 4.0;   ///< Slow fading, AR(1) per beacon.
  double shadowing_tau_s = 4.0;
  double wall_attenuation_db = 4.0;  ///< True loss per wall (the map's value may differ).
  double path_loss_exp_delta = 0.2;  ///< True exponent = surveyed exponent + delta.
  double device_offset_db = 0.0;     ///< Receiver-specific RSSI bias.
  double dropout = 0.2;
  double sensitivity_dbm = -95.0;
  std::vector<std::string> dead_beacons;
  std::map<std::string, Vec2> moved_beacons;  ///< Real position differs from the map.
};

struct Scenario {
  std::string name;
  std::string description;
  std::string venue_path;  ///< Resolved relative to the scenario file.
  std::uint64_t seed = 1;
  double truth_rate_hz = 5.0;
  WalkConfig walk;
  ImuConfig imu;
  BleConfig ble;
};

Scenario parse_scenario(std::string_view json_text, const std::string& base_dir = ".");
Scenario load_scenario_file(const std::string& path);

struct SimulationReport {
  int wall_violations = 0;  ///< Ground-truth path segments crossing a wall (scenario bug).
  double distance_m = 0.0;
  int steps = 0;
};

/// Generates a synthetic trace for `scenario` in `venue`. Deterministic for a
/// given seed on every platform.
Trace simulate(const Scenario& scenario, const Venue& venue, std::optional<std::uint64_t> seed = std::nullopt,
               SimulationReport* report = nullptr);

}  // namespace ipe::sim
