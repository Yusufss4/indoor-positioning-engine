#include "ipe/grid_engine.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    GridFilterConfig, cell_m, max_cells, max_reach_m, closed_region_fraction, closed_region_min_m2, heading_bins,
    heading_sigma_deg, heading_outlier_prob, step_length_sigma, step_length_rel_sigma, step_scale_bins,
    step_scale_spread, step_scale_sigma, step_scale_switch, estimate_rssi_offset, rssi_offset_sigma, rssi_offset_walk,
    teleport_rate_per_s, teleport_period_s, recovery_alpha_slow, recovery_alpha_fast, recovery_max_fraction,
    active_floor_rel, level_change_prob_step, level_change_prob_baro, baro_update_interval_s, baro_sigma_m,
    baro_outlier_prob, baro_offset_sigma_m, baro_offset_walk_m, level_smoothing_s, level_hysteresis,
    map_consistent_estimate, beacon_integrity, integrity_threshold_db, integrity_alpha, integrity_min_samples,
    integrity_max_sigma_m, floor_prior_weight, floor_prior_k, floor_prior_eps)

namespace {

/// `grid` with a negative floor_prior_weight replaced by the engine config's floor prior settings.
GridFilterConfig resolve(GridFilterConfig grid, const EngineConfig& base) {
  if (grid.floor_prior_weight < 0.0) {
    grid.floor_prior_weight = base.pf.floor_prior_weight;
    grid.floor_prior_k = base.pf.floor_prior_k;
    grid.floor_prior_eps = base.pf.floor_prior_eps;
  }
  return grid;
}

}  // namespace

GridEngine::GridEngine(const Venue& venue, std::shared_ptr<const GridMap> map, const EngineConfig& base,
                       const GridFilterConfig& grid)
    : venue_(&venue),
      cfg_(base),
      pdr_(base.pdr),
      filter_(std::move(map), resolve(grid, base), base.rssi, base.baro),
      smoother_(venue, base.smoothing) {
  if (&filter_.map().venue() != &venue) throw std::invalid_argument("GridEngine: the map is of another venue");
  if (!venue.rooms().empty()) {
    rooms_.emplace(venue, base.rooms);
    room_prob_.assign(venue.rooms().size(), 0.0);
  }
}

void GridEngine::on_imu(const ImuSample& sample) {
  now_ = std::max(now_, sample.t);
  if (const auto step = pdr_.process(sample)) {
    ++steps_;
    const Estimate before = filter_.estimate();
    filter_.on_step(*step);
    smoother_.on_step(step->t, before, filter_.estimate(), filter_.step_motion());
    if (step->mag_valid) {
      filter_.on_heading(step->t, step->mag_heading, deg2rad(cfg_.mag_heading_sigma_deg));
      smoother_.on_update(step->t, filter_.estimate());
    }
  }
  update_rooms(sample.t);
}

void GridEngine::on_ble(double t, std::size_t beacon_index, double rssi) {
  filter_.on_rssi(t, beacon_index, rssi);
  after_update(t);
}

void GridEngine::on_scan(double t, std::span<const RssiReading> readings) {
  filter_.on_rssi_scan(t, readings);
  after_update(t);
}

void GridEngine::on_pressure(double t, double hpa) {
  filter_.on_pressure(t, hpa);
  after_update(t);
}

void GridEngine::after_update(double t) {
  now_ = std::max(now_, t);
  smoother_.on_update(t, filter_.estimate());
  update_rooms(t);
}

void GridEngine::update_rooms(double t) {
  if (!rooms_ || !rooms_->due(t)) return;
  std::fill(room_prob_.begin(), room_prob_.end(), 0.0);
  if (filter_.estimate().valid) {
    const GridMap& map = filter_.map();
    const auto mass = filter_.mass();
    for (std::size_t c = 0; c < map.size(); ++c) {
      for (const std::uint32_t r : map.rooms(c)) room_prob_[r] += mass[c];
    }
  }
  rooms_->update(t, room_prob_);
}

void append_snapshot(const GridEngine& engine, std::vector<std::array<float, 4>>& out, std::size_t max) {
  const GridFilter& f = engine.filter();
  const auto mass = f.mass();
  std::vector<std::size_t> order(mass.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  const std::size_t k = std::min(max, order.size());
  std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(k), order.end(),
                    [&](std::size_t a, std::size_t b) { return mass[a] > mass[b] || (mass[a] == mass[b] && a < b); });
  for (std::size_t i = 0; i < k; ++i) {
    const std::size_t c = order[i];
    const Vec2 p = f.centroid(c);
    out.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(mass[c]),
                   static_cast<float>(f.map().cell(c).level)});
  }
}

namespace grid {

namespace {

void check_known_keys(const nlohmann::json& base, const nlohmann::json& patch, const std::string& path) {
  if (!patch.is_object()) return;
  for (const auto& [key, value] : patch.items()) {
    const std::string full = path.empty() ? key : path + "." + key;
    if (!base.is_object() || !base.contains(key)) throw std::invalid_argument("unknown grid config key '" + full + "'");
    check_known_keys(base.at(key), value, full);
  }
}

}  // namespace

void apply_overrides(GridFilterConfig& cfg, std::string_view json_patch) {
  nlohmann::json j = cfg;
  const auto patch = nlohmann::json::parse(json_patch);
  check_known_keys(j, patch, "");
  j.merge_patch(patch);
  cfg = j.get<GridFilterConfig>();
}

std::string config_to_json(const GridFilterConfig& cfg, int indent) { return nlohmann::json(cfg).dump(indent); }

}  // namespace grid

}  // namespace ipe
