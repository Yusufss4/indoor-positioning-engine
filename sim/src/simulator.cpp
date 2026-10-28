#include "ipe/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ipe/rng.hpp"

namespace ipe::sim {
namespace {

using nlohmann::json;

Vec2 to_vec2(const json& j) {
  if (!j.is_array() || j.size() != 2) throw std::runtime_error("scenario: expected [x, y]");
  return {j[0].get<double>(), j[1].get<double>()};
}

template <typename T>
void read(const json& j, const char* key, T& field) {
  if (j.contains(key)) field = j.at(key).get<T>();
}

}  // namespace

Scenario parse_scenario(std::string_view json_text, const std::string& base_dir) {
  const json j = json::parse(json_text);
  Scenario s;
  s.name = j.value("name", std::string("unnamed"));
  s.description = j.value("description", std::string());
  read(j, "seed", s.seed);
  read(j, "truth_rate_hz", s.truth_rate_hz);
  if (j.contains("venue")) {
    const std::filesystem::path p = j.at("venue").get<std::string>();
    s.venue_path = (p.is_absolute() ? p : std::filesystem::path(base_dir) / p).lexically_normal().string();
  }

  const json& w = j.at("walk");
  for (const auto& wp : w.at("waypoints")) s.walk.waypoints.push_back(to_vec2(wp));
  if (s.walk.waypoints.size() < 2) throw std::runtime_error("scenario: walk needs at least two waypoints");
  read(w, "step_frequency_hz", s.walk.step_frequency_hz);
  read(w, "step_length_m", s.walk.step_length_m);
  read(w, "step_length_sigma", s.walk.step_length_sigma);
  read(w, "turn_rate_deg_s", s.walk.turn_rate_deg_s);
  read(w, "waypoint_radius_m", s.walk.waypoint_radius_m);

  if (j.contains("imu")) {
    const json& m = j.at("imu");
    read(m, "rate_hz", s.imu.rate_hz);
    read(m, "accel_noise", s.imu.accel_noise);
    read(m, "gyro_noise", s.imu.gyro_noise);
    read(m, "gyro_bias", s.imu.gyro_bias);
    read(m, "mag_field_ut", s.imu.mag_field_ut);
    read(m, "mag_noise_ut", s.imu.mag_noise_ut);
    read(m, "weinberg_k", s.imu.weinberg_k);
    for (const auto& d : m.value("mag_disturbances", json::array())) {
      s.imu.mag_disturbances.push_back({to_vec2(d.at("center")), d.value("radius", 3.0), to_vec2(d.at("field"))});
    }
  }

  if (j.contains("ble")) {
    const json& b = j.at("ble");
    read(b, "rx_interval_s", s.ble.rx_interval_s);
    read(b, "rx_jitter", s.ble.rx_jitter);
    read(b, "noise_db", s.ble.noise_db);
    read(b, "shadowing_db", s.ble.shadowing_db);
    read(b, "shadowing_tau_s", s.ble.shadowing_tau_s);
    read(b, "wall_attenuation_db", s.ble.wall_attenuation_db);
    read(b, "path_loss_exp_delta", s.ble.path_loss_exp_delta);
    read(b, "device_offset_db", s.ble.device_offset_db);
    read(b, "dropout", s.ble.dropout);
    read(b, "sensitivity_dbm", s.ble.sensitivity_dbm);
    read(b, "dead_beacons", s.ble.dead_beacons);
    const json moved = b.value("moved_beacons", json::object());
    for (const auto& [id, pos] : moved.items()) s.ble.moved_beacons[id] = to_vec2(pos);
  }
  return s;
}

Scenario load_scenario_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open scenario file: " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_scenario(ss.str(), std::filesystem::path(path).parent_path().string());
}

Trace simulate(const Scenario& sc, const Venue& venue, std::optional<std::uint64_t> seed,
               SimulationReport* report) {
  Rng rng(seed.value_or(sc.seed));
  const auto& walk = sc.walk;
  const auto& imu_cfg = sc.imu;
  const auto& ble_cfg = sc.ble;

  Trace trace;
  trace.metadata["scenario"] = sc.name;
  trace.metadata["venue"] = venue.name();
  trace.metadata["seed"] = std::to_string(seed.value_or(sc.seed));
  trace.metadata["generator"] = "ipe-sim";

  SimulationReport rep;
  const double dt = 1.0 / imu_cfg.rate_hz;
  const double gravity = 9.80665;
  const double max_turn = deg2rad(walk.turn_rate_deg_s) * dt;

  Vec2 pos = walk.waypoints[0];
  double heading = std::atan2(walk.waypoints[1].y - pos.y, walk.waypoints[1].x - pos.x);
  std::size_t target = 1;
  double phase = rng.uniform();

  auto draw_step = [&] { return std::max(0.3, rng.normal(walk.step_length_m, walk.step_length_sigma)); };
  double step_len = draw_step();
  double amplitude = std::pow(step_len / imu_cfg.weinberg_k, 4.0);  // Inverse Weinberg: peak-to-peak accel.
  const double gyro_bias = imu_cfg.gyro_bias;

  // Per-beacon BLE state: true position, next reception time, shadowing.
  struct BeaconState {
    const Beacon* beacon;
    Vec2 true_pos;
    bool dead;
    double next_t;
    double last_t;
    double shadow;
  };
  std::vector<BeaconState> beacons;
  for (const auto& b : venue.beacons()) {
    BeaconState st{&b, b.pos, false, rng.uniform(0.0, ble_cfg.rx_interval_s), 0.0,
                   rng.normal(0.0, ble_cfg.shadowing_db)};
    if (auto it = ble_cfg.moved_beacons.find(b.id); it != ble_cfg.moved_beacons.end()) st.true_pos = it->second;
    st.dead = std::find(ble_cfg.dead_beacons.begin(), ble_cfg.dead_beacons.end(), b.id) != ble_cfg.dead_beacons.end();
    beacons.push_back(st);
  }

  const double truth_dt = 1.0 / sc.truth_rate_hz;
  double next_truth_t = 0.0;
  const double max_duration = 3600.0;

  const auto max_ticks = static_cast<long>(max_duration / dt);
  for (long tick = 0; tick < max_ticks; ++tick) {
    const double t = static_cast<double>(tick) * dt;
    // Pure-pursuit walker: turn toward the active waypoint at a bounded rate,
    // slowing down for sharp turns.
    while (target < walk.waypoints.size() && distance(pos, walk.waypoints[target]) < walk.waypoint_radius_m) {
      ++target;
    }
    if (target >= walk.waypoints.size()) break;
    const Vec2 goal = walk.waypoints[target];
    const double desired = std::atan2(goal.y - pos.y, goal.x - pos.x);
    const double err = wrap_angle(desired - heading);
    const double turn = std::clamp(err, -max_turn, max_turn);
    heading = wrap_angle(heading + turn);
    const double yaw_rate = turn / dt;
    const double speed = step_len * walk.step_frequency_hz * std::max(0.3, 1.0 - std::abs(err) / std::numbers::pi);
    const Vec2 next = pos + Vec2{std::cos(heading), std::sin(heading)} * (speed * dt);
    if (venue.crosses_wall(pos, next)) ++rep.wall_violations;
    rep.distance_m += distance(pos, next);
    pos = next;

    phase += walk.step_frequency_hz * dt;
    if (phase >= 1.0) {
      phase -= 1.0;
      ++rep.steps;
      step_len = draw_step();
      amplitude = std::pow(step_len / imu_cfg.weinberg_k, 4.0);
    }

    if (t + 1e-9 >= next_truth_t) {
      trace.truth.push_back({t, pos, heading});
      next_truth_t += truth_dt;
    }

    // IMU in the phone frame; the phone is held flat facing the walking direction.
    ImuSample s;
    s.t = t;
    const double vertical = 0.5 * amplitude * std::sin(2.0 * std::numbers::pi * phase);
    s.accel = {rng.normal(0.0, imu_cfg.accel_noise), rng.normal(0.0, imu_cfg.accel_noise),
               gravity + vertical + rng.normal(0.0, imu_cfg.accel_noise)};
    s.gyro = {rng.normal(0.0, imu_cfg.gyro_noise), rng.normal(0.0, imu_cfg.gyro_noise),
              yaw_rate + gyro_bias + rng.normal(0.0, imu_cfg.gyro_noise)};
    Vec2 field{0.0, imu_cfg.mag_field_ut};
    for (const auto& d : imu_cfg.mag_disturbances) {
      const double r = distance(pos, d.center);
      if (r < d.radius) field += d.field * (1.0 - r / d.radius);
    }
    const double c = std::cos(heading), sn = std::sin(heading);
    s.mag = {field.x * c + field.y * sn + rng.normal(0.0, imu_cfg.mag_noise_ut),
             -field.x * sn + field.y * c + rng.normal(0.0, imu_cfg.mag_noise_ut),
             -30.0 + rng.normal(0.0, imu_cfg.mag_noise_ut)};
    trace.imu.push_back(s);

    for (auto& b : beacons) {
      if (t < b.next_t) continue;
      const double gap = t - b.last_t;
      b.last_t = t;
      b.next_t = t + ble_cfg.rx_interval_s * (1.0 + rng.uniform(-ble_cfg.rx_jitter, ble_cfg.rx_jitter));
      const double rho = std::exp(-gap / ble_cfg.shadowing_tau_s);
      b.shadow = rho * b.shadow + std::sqrt(1.0 - rho * rho) * rng.normal(0.0, ble_cfg.shadowing_db);
      if (b.dead || rng.bernoulli(ble_cfg.dropout)) continue;

      const double d = std::max(distance(pos, b.true_pos), 0.5);
      const int walls = venue.count_wall_crossings(pos, b.true_pos);
      const double n = b.beacon->path_loss_exp + ble_cfg.path_loss_exp_delta;
      const double rssi = b.beacon->tx_power_dbm - 10.0 * n * std::log10(d) - walls * ble_cfg.wall_attenuation_db +
                          b.shadow + rng.normal(0.0, ble_cfg.noise_db) + ble_cfg.device_offset_db;
      if (rssi < ble_cfg.sensitivity_dbm) continue;
      trace.ble.push_back({t, b.beacon->id, std::round(rssi)});
    }
  }

  if (report) *report = rep;
  return trace;
}

}  // namespace ipe::sim
