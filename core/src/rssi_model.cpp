#include "ipe/rssi_model.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ipe {

double RssiModel::expected_rssi_free_space(const Beacon& b, double d) const {
  return b.tx_power_dbm - 10.0 * b.path_loss_exp * std::log10(std::max(d, cfg_.min_distance));
}

double RssiModel::expected_rssi(const Beacon& b, Vec2 p, int level, double wall_db, double floor_db) const {
  double rssi = 0.0;
  if (b.level == level) {
    rssi = expected_rssi_free_space(b, distance(p, b.pos));
  } else {
    const double dz = venue_->level(b.level).elevation_m - venue_->level(level).elevation_m;
    rssi = expected_rssi_free_space(b, std::hypot(distance(p, b.pos), dz)) - floor_db * std::abs(b.level - level);
  }
  if (cfg_.wall_aware) rssi -= wall_db * venue_->count_wall_crossings(p, b.pos, level);
  return rssi;
}

double RssiModel::log_likelihood(double residual_db) const {
  const double s = cfg_.sigma_db;
  const double z = residual_db / s;
  const double gauss = std::exp(-0.5 * z * z) / (s * std::sqrt(2.0 * std::numbers::pi));
  const double uniform = 1.0 / cfg_.outlier_range_db;
  return std::log((1.0 - cfg_.outlier_prob) * gauss + cfg_.outlier_prob * uniform);
}

}  // namespace ipe
