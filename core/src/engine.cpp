#include "ipe/engine.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace ipe {

double Estimate::sigma_radius() const {
  const double det = cov_xx * cov_yy - cov_xy * cov_xy;
  return det > 0.0 ? std::sqrt(std::sqrt(det)) : 0.0;
}

EngineConfig recommended_config(const Venue& venue) {
  EngineConfig cfg;
  const FloorFingerprints& scans = venue.floor_fingerprints();
  // A radio map fitted from recorded walks (the venue says so, or carries the scans): a wider RSSI spread
  // than surveyed beacons have, and corrections drawn no faster than a walk.
  if (venue.radio_map_fitted() || !scans.empty()) {
    cfg.rssi.sigma_db = 8.0;
    cfg.smoothing.max_correction_mps = 1.5;
  }
  // Training scans vote on the floor (it only acts on venues with several levels); with positions they
  // are fingerprints too.
  if (!scans.empty()) cfg.filter.floor_prior_weight = 3.0;
  if (scans.has_positions()) cfg.filter.fingerprint_weight = 3.0;
  return cfg;
}

PositioningEngine::PositioningEngine(const Venue& venue) : PositioningEngine(venue, recommended_config(venue)) {}

namespace {

/// Spread of a step's magnetometer heading [deg]: indoors the field is bent by steel and wiring.
constexpr double kMagHeadingSigmaDeg = 12.0;

PdrConfig pdr_params(const EngineConfig& cfg) {
  PdrConfig p;
  p.weinberg_k = cfg.pdr.weinberg_k;
  return p;
}

RssiModelConfig rssi_params(const EngineConfig& cfg) {
  RssiModelConfig p;
  p.sigma_db = cfg.rssi.sigma_db;
  p.wall_aware = cfg.rssi.wall_aware;
  return p;
}

TrackSmootherConfig smoother_params(const EngineConfig& cfg) {
  TrackSmootherConfig p;
  p.enabled = cfg.smoothing.enabled;
  p.tau_s = cfg.smoothing.tau_s;
  p.max_correction_mps = cfg.smoothing.max_correction_mps;
  p.snap_m = cfg.smoothing.snap_m;
  return p;
}

RoomEventConfig room_params(const EngineConfig& cfg) {
  RoomEventConfig p;
  p.enter_prob = cfg.rooms.enter_prob;
  p.exit_prob = cfg.rooms.exit_prob;
  return p;
}

}  // namespace

GridFilterConfig filter_params(const EngineConfig& cfg) {
  GridFilterConfig p;
  p.cell_m = cfg.filter.cell_m;
  p.active_floor = cfg.filter.active_floor;
  p.estimate_rssi_offset = cfg.filter.estimate_rssi_offset;
  p.beacon_integrity = cfg.filter.beacon_integrity;
  p.floor_prior_weight = cfg.filter.floor_prior_weight;
  p.fingerprint_weight = cfg.filter.fingerprint_weight;
  return p;
}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg)
    : PositioningEngine(venue, cfg, std::make_shared<const GridMap>(venue, filter_params(cfg))) {}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg, std::shared_ptr<const GridMap> map)
    : venue_(&venue),
      cfg_(cfg),
      pdr_(pdr_params(cfg)),
      filter_(std::move(map), filter_params(cfg), rssi_params(cfg)),
      smoother_(venue, smoother_params(cfg)) {
  if (&filter_.map().venue() != &venue) throw std::invalid_argument("PositioningEngine: the map is of another venue");
  if (!venue.rooms().empty()) {
    rooms_.emplace(venue, room_params(cfg));
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
    filter_.on_heading(step.t, step.mag_heading, deg2rad(kMagHeadingSigmaDeg));
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
    for (const std::uint32_t c : filter_.active_cells()) {
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
  const auto active = f.active_cells();
  std::vector<std::size_t> order(active.begin(), active.end());
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
