#include "ipe/engine.hpp"

#include <cmath>
#include <numbers>

namespace ipe {

double Estimate::sigma_radius() const {
  const double det = cov_xx * cov_yy - cov_xy * cov_xy;
  return det > 0.0 ? std::sqrt(std::sqrt(det)) : 0.0;
}

PositioningEngine::PositioningEngine(const Venue& venue, EngineConfig cfg)
    : venue_(&venue), cfg_(cfg), pdr_(cfg.pdr), pf_(venue, cfg.pf, cfg.rssi, cfg.baro) {
  if (!venue.rooms().empty()) rooms_.emplace(venue, cfg.rooms);
}

void PositioningEngine::on_imu(const ImuSample& sample) {
  if (const auto step = pdr_.process(sample)) {
    ++steps_;
    pf_.on_step(*step);
    if (step->mag_valid) pf_.on_heading(step->t, step->mag_heading, deg2rad(cfg_.mag_heading_sigma_deg));
  }
  update_rooms(sample.t);
}

void PositioningEngine::on_ble(double t, std::string_view beacon_id, double rssi) {
  if (const auto idx = venue_->beacon_index(beacon_id)) pf_.on_rssi(t, *idx, rssi);
  update_rooms(t);
}

void PositioningEngine::on_ble(double t, std::size_t beacon_index, double rssi) {
  pf_.on_rssi(t, beacon_index, rssi);
  update_rooms(t);
}

void PositioningEngine::on_scan(double t, std::span<const RssiReading> readings) {
  pf_.on_rssi_scan(t, readings);
  update_rooms(t);
}

void PositioningEngine::on_pressure(double t, double hpa) {
  pf_.on_pressure(t, hpa);
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
