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

struct Waypoint {
  Vec2 pos;
  int level = 0;  ///< Level *ordinal* as written in the venue file; resolved against the venue in simulate().
};

struct WalkConfig {
  /// Consecutive waypoints on different levels must both lie inside one
  /// connector serving both levels: the walker climbs the stairs between them
  /// or rides the lift from the first to the second.
  std::vector<Waypoint> waypoints;
  double step_frequency_hz = 1.8;
  double step_length_m = 0.7;
  double step_length_sigma = 0.04;
  double turn_rate_deg_s = 150.0;
  double waypoint_radius_m = 0.3;
  double stairs_speed_factor = 0.6;  ///< Horizontal speed on stairs relative to level walking.
  double elevator_speed_mps = 1.5;
  double elevator_accel_mps2 = 0.6;
  double elevator_dwell_s = 4.0;  ///< Standing in the cabin before and after the ride.
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
  double wall_attenuation_db = 4.0;    ///< True loss per wall (the map's value may differ).
  double floor_attenuation_db = 14.0;  ///< True loss per slab between beacon and phone levels.
  double path_loss_exp_delta = 0.2;    ///< True exponent = surveyed exponent + delta.
  double device_offset_db = 0.0;       ///< Receiver-specific RSSI bias.
  double dropout = 0.2;
  double sensitivity_dbm = -95.0;
  std::vector<std::string> dead_beacons;
  std::map<std::string, Vec2> moved_beacons;  ///< Real position differs from the map.
};

/// Phone barometer. Pressure follows the walker's height through the
/// barometric formula, on top of weather drift, a device bias, white noise
/// and short spikes from doors and ventilation.
struct BaroSensorConfig {
  bool enabled = false;  ///< True when the scenario has a "baro" section.
  double rate_hz = 5.0;
  double noise_hpa = 0.03;
  double reference_hpa = 1013.25;   ///< True sea-level-equivalent pressure at t = 0.
  double drift_hpa_per_hour = 0.0;  ///< Weather trend.
  double drift_walk_hpa = 0.01;     ///< Random-walk spread of the weather per minute.
  double device_offset_hpa = 0.0;   ///< Constant sensor bias.
  double spike_rate_per_min = 0.5;  ///< Door/HVAC pressure transients.
  double spike_hpa = 0.12;          ///< Spread of the spike amplitude.
  double spike_tau_s = 1.5;         ///< Spike decay time constant.
  double phone_height_m = 1.2;      ///< Phone height above the walking surface.
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
  BaroSensorConfig baro;
};

Scenario parse_scenario(std::string_view json_text, const std::string& base_dir = ".");
Scenario load_scenario_file(const std::string& path);

struct SimulationReport {
  int wall_violations = 0;  ///< Ground-truth path segments crossing a wall (scenario bug).
  double distance_m = 0.0;
  int steps = 0;
  int level_changes = 0;
};

/// Generates a synthetic trace for `scenario` in `venue`. Deterministic for a
/// given seed on every platform. Throws if the walk changes level outside a
/// connector or references a level the venue does not have.
Trace simulate(const Scenario& scenario, const Venue& venue, std::optional<std::uint64_t> seed = std::nullopt,
               SimulationReport* report = nullptr);

}  // namespace ipe::sim
