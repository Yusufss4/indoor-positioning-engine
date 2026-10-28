#include "ipe/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ipe/baro.hpp"
#include "ipe/rng.hpp"

namespace ipe::sim {
namespace {

using nlohmann::json;

Vec2 to_vec2(const json& j) {
  if (!j.is_array() || j.size() != 2) throw std::runtime_error("scenario: expected [x, y]");
  return {j[0].get<double>(), j[1].get<double>()};
}

Waypoint to_waypoint(const json& j) {
  if (!j.is_array() || (j.size() != 2 && j.size() != 3)) throw std::runtime_error("scenario: expected [x, y(, level)]");
  return {{j[0].get<double>(), j[1].get<double>()}, j.size() == 3 ? j[2].get<int>() : 0};
}

template <typename T>
void read(const json& j, const char* key, T& field) {
  if (j.contains(key)) field = j.at(key).get<T>();
}

/// Vertical lift motion: dwell, trapezoidal (or triangular) speed profile, dwell.
struct LiftRide {
  double from = 0.0, to = 0.0, dwell = 0.0, accel = 0.6;
  double t_acc = 0.0, t_cruise = 0.0, v_peak = 0.0;

  LiftRide(double from_z, double to_z, const WalkConfig& w)
      : from(from_z), to(to_z), dwell(w.elevator_dwell_s), accel(w.elevator_accel_mps2) {
    const double dist = std::abs(to - from);
    t_acc = w.elevator_speed_mps / accel;
    if (accel * t_acc * t_acc > dist) {
      t_acc = std::sqrt(dist / accel);
      t_cruise = 0.0;
    } else {
      t_cruise = (dist - accel * t_acc * t_acc) / w.elevator_speed_mps;
    }
    v_peak = accel * t_acc;
  }

  [[nodiscard]] double duration() const { return 2.0 * dwell + 2.0 * t_acc + t_cruise; }

  /// Height and vertical acceleration `tau` seconds after boarding.
  void at(double tau, double& z, double& acc) const {
    const double sign = to >= from ? 1.0 : -1.0;
    const double d_acc = 0.5 * accel * t_acc * t_acc;
    double s = 0.0;
    acc = 0.0;
    const double u = tau - dwell;
    if (u <= 0.0) {
      s = 0.0;
    } else if (u < t_acc) {
      s = 0.5 * accel * u * u;
      acc = accel;
    } else if (u < t_acc + t_cruise) {
      s = d_acc + v_peak * (u - t_acc);
    } else if (u < 2.0 * t_acc + t_cruise) {
      const double r = u - t_acc - t_cruise;
      s = d_acc + v_peak * t_cruise + v_peak * r - 0.5 * accel * r * r;
      acc = -accel;
    } else {
      s = std::abs(to - from);
    }
    z = from + sign * s;
    acc *= sign;
  }
};

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
  for (const auto& wp : w.at("waypoints")) s.walk.waypoints.push_back(to_waypoint(wp));
  if (s.walk.waypoints.size() < 2) throw std::runtime_error("scenario: walk needs at least two waypoints");
  read(w, "step_frequency_hz", s.walk.step_frequency_hz);
  read(w, "step_length_m", s.walk.step_length_m);
  read(w, "step_length_sigma", s.walk.step_length_sigma);
  read(w, "turn_rate_deg_s", s.walk.turn_rate_deg_s);
  read(w, "waypoint_radius_m", s.walk.waypoint_radius_m);
  read(w, "stairs_speed_factor", s.walk.stairs_speed_factor);
  read(w, "elevator_speed_mps", s.walk.elevator_speed_mps);
  read(w, "elevator_accel_mps2", s.walk.elevator_accel_mps2);
  read(w, "elevator_dwell_s", s.walk.elevator_dwell_s);

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
    read(b, "floor_attenuation_db", s.ble.floor_attenuation_db);
    read(b, "path_loss_exp_delta", s.ble.path_loss_exp_delta);
    read(b, "device_offset_db", s.ble.device_offset_db);
    read(b, "dropout", s.ble.dropout);
    read(b, "sensitivity_dbm", s.ble.sensitivity_dbm);
    read(b, "dead_beacons", s.ble.dead_beacons);
    const json moved = b.value("moved_beacons", json::object());
    for (const auto& [id, pos] : moved.items()) s.ble.moved_beacons[id] = to_vec2(pos);
  }

  if (j.contains("baro")) {
    const json& p = j.at("baro");
    s.baro.enabled = p.value("enabled", true);
    read(p, "rate_hz", s.baro.rate_hz);
    read(p, "noise_hpa", s.baro.noise_hpa);
    read(p, "reference_hpa", s.baro.reference_hpa);
    read(p, "drift_hpa_per_hour", s.baro.drift_hpa_per_hour);
    read(p, "drift_walk_hpa", s.baro.drift_walk_hpa);
    read(p, "device_offset_hpa", s.baro.device_offset_hpa);
    read(p, "spike_rate_per_min", s.baro.spike_rate_per_min);
    read(p, "spike_hpa", s.baro.spike_hpa);
    read(p, "spike_tau_s", s.baro.spike_tau_s);
    read(p, "phone_height_m", s.baro.phone_height_m);
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

Trace simulate(const Scenario& sc, const Venue& venue, std::optional<std::uint64_t> seed, SimulationReport* report) {
  const std::uint64_t run_seed = seed.value_or(sc.seed);
  Rng rng(run_seed);
  const auto& walk = sc.walk;
  const auto& imu_cfg = sc.imu;
  const auto& ble_cfg = sc.ble;
  const auto& baro_cfg = sc.baro;
  const auto& wps = walk.waypoints;

  // Resolve waypoint levels and check that every level change happens inside a connector.
  std::vector<int> wl(wps.size());
  for (std::size_t k = 0; k < wps.size(); ++k) {
    const auto idx = venue.level_index(wps[k].level);
    if (!idx) throw std::runtime_error("scenario: waypoint " + std::to_string(k) + " on unknown level");
    wl[k] = *idx;
  }
  for (std::size_t k = 0; k + 1 < wps.size(); ++k) {
    if (wl[k] == wl[k + 1]) continue;
    const Connector* c = venue.connector_at(wps[k].pos, wl[k]);
    if (!c || !c->serves(wl[k + 1]) || !c->contains(wps[k + 1].pos)) {
      throw std::runtime_error("scenario: level change between waypoints " + std::to_string(k) + " and " +
                               std::to_string(k + 1) + " is not inside a connector serving both levels");
    }
  }

  Trace trace;
  trace.metadata["scenario"] = sc.name;
  trace.metadata["venue"] = venue.name();
  trace.metadata["seed"] = std::to_string(run_seed);
  trace.metadata["generator"] = "ipe-sim";

  SimulationReport rep;
  const double dt = 1.0 / imu_cfg.rate_hz;
  const double gravity = 9.80665;
  const double max_turn = deg2rad(walk.turn_rate_deg_s) * dt;

  Vec2 pos = wps[0].pos;
  double heading = std::atan2(wps[1].pos.y - pos.y, wps[1].pos.x - pos.x);
  std::size_t target = 1;
  double phase = rng.uniform();

  // Vertical state: the level index the walker belongs to and the height of
  // the walking surface. Between levels (stairs, lift) the reported level is
  // the nearest floor.
  enum class Mode { Walk, Climb, Ride };
  Mode mode = Mode::Walk;
  int level = wl[0];
  double z = venue.level(level).elevation_m;
  double climb_from = 0.0, climb_to = 0.0, climb_len = 1.0;
  int pending_level = level;
  std::optional<LiftRide> ride;
  double ride_start_t = 0.0;

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
    BeaconState st{
        &b, b.pos, false, rng.uniform(0.0, ble_cfg.rx_interval_s), 0.0, rng.normal(0.0, ble_cfg.shadowing_db)};
    if (auto it = ble_cfg.moved_beacons.find(b.id); it != ble_cfg.moved_beacons.end()) st.true_pos = it->second;
    st.dead = std::find(ble_cfg.dead_beacons.begin(), ble_cfg.dead_beacons.end(), b.id) != ble_cfg.dead_beacons.end();
    beacons.push_back(st);
  }

  // The barometer has its own generator so enabling it leaves the IMU and
  // BLE streams of a scenario unchanged.
  Rng baro_rng(run_seed ^ 0xba20'5eed'0000'0001ULL);
  const double baro_dt = baro_cfg.rate_hz > 0.0 ? 1.0 / baro_cfg.rate_hz : 1e300;
  double next_baro_t = 0.0;
  double weather_walk = 0.0;
  double spike = 0.0;

  const double truth_dt = 1.0 / sc.truth_rate_hz;
  double next_truth_t = 0.0;
  const double max_duration = 3600.0;

  const auto max_ticks = static_cast<long>(max_duration / dt);
  for (long tick = 0; tick < max_ticks; ++tick) {
    const double t = static_cast<double>(tick) * dt;
    double yaw_rate = 0.0;
    double lift_accel = 0.0;
    bool stepping = true;

    if (mode != Mode::Ride) {
      // Pure-pursuit walker: turn toward the active waypoint at a bounded rate,
      // slowing down for sharp turns.
      while (target < wps.size() && distance(pos, wps[target].pos) < walk.waypoint_radius_m) {
        if (mode == Mode::Climb) {
          mode = Mode::Walk;
          level = pending_level;
          z = climb_to;
          ++rep.level_changes;
        }
        if (target + 1 < wps.size() && wl[target + 1] != wl[target]) {
          const Connector* c = venue.connector_at(wps[target].pos, wl[target]);
          ++target;
          pending_level = wl[target];
          if (c->kind == ConnectorKind::Elevator) {
            mode = Mode::Ride;
            ride.emplace(z, venue.level(pending_level).elevation_m, walk);
            ride_start_t = t;
            break;
          }
          mode = Mode::Climb;
          climb_from = z;
          climb_to = venue.level(pending_level).elevation_m;
          climb_len = std::max(distance(pos, wps[target].pos), 0.1);
          continue;
        }
        ++target;
      }
      if (target >= wps.size()) break;
    }

    if (mode == Mode::Ride) {
      stepping = false;
      const double tau = t - ride_start_t;
      ride->at(tau, z, lift_accel);
      if (tau >= ride->duration()) {
        mode = Mode::Walk;
        level = pending_level;
        z = ride->to;
        lift_accel = 0.0;
        ++rep.level_changes;
      }
    } else {
      const Vec2 goal = wps[target].pos;
      const double desired = std::atan2(goal.y - pos.y, goal.x - pos.x);
      const double err = wrap_angle(desired - heading);
      const double turn = std::clamp(err, -max_turn, max_turn);
      heading = wrap_angle(heading + turn);
      yaw_rate = turn / dt;
      double speed = step_len * walk.step_frequency_hz * std::max(0.3, 1.0 - std::abs(err) / std::numbers::pi);
      if (mode == Mode::Climb) speed *= walk.stairs_speed_factor;
      const Vec2 next = pos + Vec2{std::cos(heading), std::sin(heading)} * (speed * dt);
      const int wall_level = mode == Mode::Climb ? venue.nearest_level(z) : level;
      if (venue.crosses_wall(pos, next, wall_level)) ++rep.wall_violations;
      rep.distance_m += distance(pos, next);
      pos = next;
      if (mode == Mode::Climb) {
        const double progress = std::clamp(1.0 - distance(pos, goal) / climb_len, 0.0, 1.0);
        z = climb_from + (climb_to - climb_from) * progress;
      }
    }
    const int cur_level = mode == Mode::Walk ? level : venue.nearest_level(z);

    if (stepping) {
      phase += walk.step_frequency_hz * dt;
      if (phase >= 1.0) {
        phase -= 1.0;
        ++rep.steps;
        step_len = draw_step();
        amplitude = std::pow(step_len / imu_cfg.weinberg_k, 4.0);
      }
    }

    if (t + 1e-9 >= next_truth_t) {
      trace.truth.push_back({t, pos, heading, cur_level, z});
      next_truth_t += truth_dt;
    }

    // IMU in the phone frame; the phone is held flat facing the walking direction.
    ImuSample s;
    s.t = t;
    const double vertical = (stepping ? 0.5 * amplitude * std::sin(2.0 * std::numbers::pi * phase) : 0.0) + lift_accel;
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

      double range = distance(pos, b.true_pos);
      double slab_loss = 0.0;
      if (venue.multi_level()) {
        range = std::hypot(range, venue.level(b.beacon->level).elevation_m - z);
        slab_loss = ble_cfg.floor_attenuation_db * std::abs(b.beacon->level - cur_level);
      }
      const double d = std::max(range, 0.5);
      const int walls = venue.count_wall_crossings(pos, b.true_pos, cur_level);
      const double n = b.beacon->path_loss_exp + ble_cfg.path_loss_exp_delta;
      const double rssi = b.beacon->tx_power_dbm - 10.0 * n * std::log10(d) - walls * ble_cfg.wall_attenuation_db -
                          slab_loss + b.shadow + rng.normal(0.0, ble_cfg.noise_db) + ble_cfg.device_offset_db;
      if (rssi < ble_cfg.sensitivity_dbm) continue;
      trace.ble.push_back({t, b.beacon->id, std::round(rssi)});
    }

    if (baro_cfg.enabled && t + 1e-9 >= next_baro_t) {
      weather_walk += baro_cfg.drift_walk_hpa * std::sqrt(baro_dt / 60.0) * baro_rng.normal();
      spike *= std::exp(-baro_dt / baro_cfg.spike_tau_s);
      if (baro_rng.bernoulli(baro_cfg.spike_rate_per_min * baro_dt / 60.0)) {
        spike += baro_rng.normal(0.0, baro_cfg.spike_hpa);
      }
      const double weather = baro_cfg.reference_hpa + baro_cfg.drift_hpa_per_hour * t / 3600.0 + weather_walk;
      const double hpa = altitude_to_pressure(z + baro_cfg.phone_height_m, weather) + baro_cfg.device_offset_hpa +
                         spike + baro_rng.normal(0.0, baro_cfg.noise_hpa);
      trace.baro.push_back({t, hpa});
      next_baro_t += baro_dt;
    }
  }

  if (report) *report = rep;
  return trace;
}

}  // namespace ipe::sim
