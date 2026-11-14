#include "ipe/engine.hpp"

#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace ipe {

double Estimate::sigma_radius() const {
  const double det = cov_xx * cov_yy - cov_xy * cov_xy;
  return det > 0.0 ? std::sqrt(std::sqrt(det)) : 0.0;
}

EngineConfig recommended_config(const Venue& venue) {
  EngineConfig cfg;
  const FloorFingerprints& scans = venue.floor_fingerprints();
  if (scans.empty()) return cfg;
  // A radio map fitted from training scans: a wider RSSI spread than surveyed beacons have, and corrections
  // drawn no faster than a walk. The floor vote only acts on venues with several levels.
  cfg.rssi.sigma_db = 8.0;
  cfg.smoothing.max_correction_mps = 1.5;
  cfg.filter.floor_prior_weight = 3.0;
  if (scans.has_positions()) cfg.filter.fingerprint_weight = 3.0;
  return cfg;
}

PositioningEngine::PositioningEngine(const Venue& venue) : PositioningEngine(venue, recommended_config(venue)) {}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg)
    : PositioningEngine(venue, cfg, std::make_shared<const GridMap>(venue, cfg.filter)) {}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg, std::shared_ptr<const GridMap> map)
    : venue_(&venue),
      cfg_(cfg),
      pdr_(cfg.pdr),
      filter_(std::move(map), cfg.filter, cfg.rssi, cfg.baro),
      smoother_(venue, cfg.smoothing) {
  if (&filter_.map().venue() != &venue) throw std::invalid_argument("PositioningEngine: the map is of another venue");
  if (!venue.rooms().empty()) {
    rooms_.emplace(venue, cfg.rooms);
    room_prob_.assign(venue.rooms().size(), 0.0);
  }
}

void PositioningEngine::on_imu(const ImuSample& sample) {
  now_ = std::max(now_, sample.t);
  if (const auto step = pdr_.process(sample)) apply_step(*step);
  update_rooms(sample.t);
}

void PositioningEngine::on_step(const StepEvent& step) {
  now_ = std::max(now_, step.t);
  apply_step(step);
  update_rooms(step.t);
}

void PositioningEngine::on_heading(double t, double heading, double sigma) {
  filter_.on_heading(t, heading, sigma);
  after_update(t);
}

void PositioningEngine::apply_step(const StepEvent& step) {
  ++steps_;
  const Estimate before = filter_.estimate();
  filter_.on_step(step);
  smoother_.on_step(step.t, before, filter_.estimate(), filter_.step_motion());
  if (step.mag_valid) {
    filter_.on_heading(step.t, step.mag_heading, deg2rad(cfg_.mag_heading_sigma_deg));
    smoother_.on_update(step.t, filter_.estimate());
  }
}

void PositioningEngine::on_ble(double t, std::string_view beacon_id, double rssi) {
  if (const auto idx = venue_->beacon_index(beacon_id)) filter_.on_rssi(t, *idx, rssi);
  after_update(t);
}

void PositioningEngine::on_ble(double t, std::size_t beacon_index, double rssi) {
  filter_.on_rssi(t, beacon_index, rssi);
  after_update(t);
}

void PositioningEngine::on_scan(double t, std::span<const RssiReading> readings) {
  filter_.on_rssi_scan(t, readings);
  after_update(t);
}

void PositioningEngine::on_pressure(double t, double hpa) {
  filter_.on_pressure(t, hpa);
  after_update(t);
}

void PositioningEngine::on_position_fix(double t, Vec2 pos, int level, double sigma_m) {
  filter_.on_position_fix(t, pos, level, sigma_m);
  after_update(t);
}

void PositioningEngine::after_update(double t) {
  now_ = std::max(now_, t);
  smoother_.on_update(t, filter_.estimate());
  update_rooms(t);
}

void PositioningEngine::update_rooms(double t) {
  if (!rooms_ || !rooms_->due(t)) return;
  // A room's probability is the mass in its cells; without a fix every room is at 0.
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

std::vector<RoomEvent> PositioningEngine::take_room_events() {
  return rooms_ ? rooms_->take_events() : std::vector<RoomEvent>{};
}

std::vector<std::size_t> PositioningEngine::current_rooms() const {
  std::vector<std::size_t> out;
  if (rooms_) rooms_->current_rooms(out);
  return out;
}

void append_snapshot(const PositioningEngine& engine, std::vector<std::array<float, 4>>& out, std::size_t max) {
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

}  // namespace ipe
