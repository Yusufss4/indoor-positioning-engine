#include "ipe/engine.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace ipe {

double Estimate::sigma_radius() const {
  const double det = cov_xx * cov_yy - cov_xy * cov_xy;
  return det > 0.0 ? std::sqrt(std::sqrt(det)) : 0.0;
}

std::string_view to_string(FilterKind kind) {
  switch (kind) {
    case FilterKind::ParticleFilter:
      return "pf";
    case FilterKind::Ekf:
      return "ekf";
    case FilterKind::WeightedCentroid:
      return "wcl";
  }
  return "unknown";
}

FilterKind parse_filter_kind(std::string_view name) {
  if (name == "pf") return FilterKind::ParticleFilter;
  if (name == "ekf") return FilterKind::Ekf;
  if (name == "wcl") return FilterKind::WeightedCentroid;
  throw std::invalid_argument("unknown filter '" + std::string(name) + "' (expected pf, ekf or wcl)");
}

std::unique_ptr<Filter> make_filter(const Venue& venue, const EngineConfig& cfg) {
  switch (cfg.filter) {
    case FilterKind::ParticleFilter:
      return std::make_unique<ParticleFilter>(venue, cfg.pf, cfg.rssi, cfg.baro);
    case FilterKind::Ekf:
      return std::make_unique<Ekf>(venue, cfg.ekf, cfg.rssi, cfg.baro);
    case FilterKind::WeightedCentroid:
      return std::make_unique<WeightedCentroid>(venue, cfg.wcl, cfg.rssi);
  }
  throw std::invalid_argument("unhandled filter kind");
}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg)
    : venue_(&venue),
      cfg_(cfg),
      pdr_(cfg.pdr),
      filter_(make_filter(venue, cfg)),
      pf_(dynamic_cast<const ParticleFilter*>(filter_.get())) {
  if (!venue.rooms().empty()) rooms_.emplace(venue, cfg.rooms);
}

void PositioningEngine::on_imu(const ImuSample& sample) {
  if (const auto step = pdr_.process(sample)) {
    ++steps_;
    filter_->on_step(*step);
    if (step->mag_valid) filter_->on_heading(step->t, step->mag_heading, deg2rad(cfg_.mag_heading_sigma_deg));
  }
  update_rooms(sample.t);
}

void PositioningEngine::on_ble(double t, std::string_view beacon_id, double rssi) {
  if (const auto idx = venue_->beacon_index(beacon_id)) filter_->on_rssi(t, *idx, rssi);
  update_rooms(t);
}

void PositioningEngine::on_ble(double t, std::size_t beacon_index, double rssi) {
  filter_->on_rssi(t, beacon_index, rssi);
  update_rooms(t);
}

void PositioningEngine::on_scan(double t, std::span<const RssiReading> readings) {
  filter_->on_rssi_scan(t, readings);
  update_rooms(t);
}

void PositioningEngine::on_pressure(double t, double hpa) {
  filter_->on_pressure(t, hpa);
  update_rooms(t);
}

std::vector<RoomEvent> PositioningEngine::take_room_events() {
  return rooms_ ? rooms_->take_events() : std::vector<RoomEvent>{};
}

std::vector<std::size_t> PositioningEngine::current_rooms() const {
  std::vector<std::size_t> out;
  if (rooms_) rooms_->current_rooms(out);
  return out;
}

}  // namespace ipe
