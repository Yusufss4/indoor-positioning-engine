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
    : venue_(&venue), cfg_(cfg), pdr_(cfg.pdr), filter_(make_filter(venue, cfg)) {}

void PositioningEngine::on_imu(const ImuSample& sample) {
  const auto step = pdr_.process(sample);
  if (!step) return;
  ++steps_;
  filter_->on_step(*step);
  if (step->mag_valid) filter_->on_heading(step->t, step->mag_heading, deg2rad(cfg_.mag_heading_sigma_deg));
}

void PositioningEngine::on_ble(double t, std::string_view beacon_id, double rssi) {
  if (const auto idx = venue_->beacon_index(beacon_id)) filter_->on_rssi(t, *idx, rssi);
}

void PositioningEngine::on_ble(double t, std::size_t beacon_index, double rssi) {
  filter_->on_rssi(t, beacon_index, rssi);
}

void PositioningEngine::on_pressure(double t, double hpa) { filter_->on_pressure(t, hpa); }

const ParticleFilter* PositioningEngine::particle_filter() const {
  return dynamic_cast<const ParticleFilter*>(filter_.get());
}

}  // namespace ipe
