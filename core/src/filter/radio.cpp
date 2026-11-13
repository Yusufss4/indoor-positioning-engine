// Radio: the integrity gate, a scan's readings and the log-distance model's term of the update.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

bool GridFilter::check_beacon(std::size_t beacon_index, double rssi) {
  if (!cfg_.beacon_integrity) return true;
  if (!integrity_.can_judge(cloud_.sigma_radius())) return !integrity_.excluded(beacon_index);
  const Beacon& beacon = map_->venue().beacons()[beacon_index];
  const double expected = rssi_model_.expected_rssi(beacon, cloud_.pos, cloud_.level) + mean_rssi_offset_;
  return integrity_.admit(beacon_index, rssi - expected);
}

void GridFilter::on_rssi(double t, std::size_t beacon_index, double rssi) {
  if (beacon_index >= map_->venue().beacons().size() || !map_->venue().beacons()[beacon_index].modelled) return;
  now_ = t;
  if (!check_beacon(beacon_index, rssi)) return;
  const RssiReading reading{beacon_index, rssi};
  update_rssi(t, std::span<const RssiReading>(&reading, 1));
}

void GridFilter::on_rssi_scan(double t, std::span<const RssiReading> readings) {
  now_ = t;
  scan_.clear();
  for (const auto& r : readings) {
    if (r.beacon_index >= map_->venue().beacons().size() || !map_->venue().beacons()[r.beacon_index].modelled) continue;
    if (!check_beacon(r.beacon_index, r.rssi)) continue;
    scan_.push_back(r);
  }
  // The fingerprints and the floor vote compare the scan as it was heard: every beacon, also those
  // without a model and those the integrity monitor distrusts.
  neighbour_count_ = cfg_.fingerprint_weight > 0.0 ? fingerprint_map_.nearest(readings, neighbours_) : 0;
  if (scan_.empty() && neighbour_count_ == 0) return;
  floor_prior_active_ = multi_level_ && cfg_.floor_prior_weight > 0.0 && floor_classifier_.classify(readings, floor_p_);
  update_rssi(t, scan_);
  floor_prior_active_ = false;
  neighbour_count_ = 0;
}

double GridFilter::expected_rssi(std::size_t beacon, std::size_t cell, Vec2 pos) const {
  const Venue& v = map_->venue();
  const Beacon& b = v.beacons()[beacon];
  const int level = map_->cell(cell).level;
  double rssi = 0.0;
  if (b.level == level) {
    rssi = rssi_model_.expected_rssi_free_space(b, distance(pos, b.pos));
  } else {
    const double dz = elevation(b.level) - elevation(level);
    rssi = rssi_model_.expected_rssi_free_space(b, std::hypot(distance(pos, b.pos), dz)) -
           v.floor_attenuation_db() * std::abs(b.level - level);
  }
  return rssi;
}

void GridFilter::update_rssi(double t, std::span<const RssiReading> readings) {
  // One radio update is a sum of log-likelihood terms per cell, applied together. A new measurement of the
  // same scan is another add_*_loglik() here.
  std::fill(loglik_.begin(), loglik_.end(), 0.0);
  add_radio_model_loglik(readings);
  if (neighbour_count_ > 0) add_fingerprint_loglik();
  if (floor_prior_active_) add_floor_vote_loglik();
  localized_ = true;
  apply_cell_loglik(t);
}

/// The log-distance model: each reading against the RSSI expected at the cell, through its walls, with the
/// cell's device offset, which the reading also updates.
void GridFilter::add_radio_model_loglik(std::span<const RssiReading> readings) {
  const RssiModelConfig& rc = rssi_model_.config();
  const double noise_var = rc.sigma_db * rc.sigma_db;
  const double outlier_density = 1.0 / rc.outlier_range_db;
  const double wall_db = map_->venue().wall_attenuation_db();
  const double walk2 = cfg_.rssi_offset_walk * cfg_.rssi_offset_walk;
  const std::size_t n = map_->size();
  for (const auto& r : readings) {
    const std::span<const std::uint8_t> walls =
        rc.wall_aware ? map_->wall_counts(r.beacon_index) : std::span<const std::uint8_t>{};
    for (std::size_t c = 0; c < n; ++c) {
      if (mass_[c] <= 0.0) continue;
      double pred = expected_rssi(r.beacon_index, c, {cx_[c], cy_[c]}) + dev_mu_[c];
      if (!walls.empty()) pred -= wall_db * walls[c];
      const double res = r.rssi - pred;
      double resp = 0.0;
      const double lik = mixture(res, noise_var + dev_var_[c], rc.outlier_prob, outlier_density, resp);
      loglik_[c] += std::log(lik);
      if (cfg_.estimate_rssi_offset) adf_update(dev_mu_[c], dev_var_[c], res, noise_var, resp);
    }
  }
  if (cfg_.estimate_rssi_offset) {
    for (std::size_t c = 0; c < n; ++c) dev_var_[c] += walk2;
  }
}

}  // namespace ipe
