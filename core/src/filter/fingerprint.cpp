// What a scan's fingerprint adds to a radio update: the nearest training scans and the floor vote.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

/// The scan's nearest training scans as a kernel density over position and level, mixed with the uniform
/// prior over the cells. The log-distance model reads the same scan, so what it has added counts for less
/// next to them (fingerprint_model_weight).
void GridFilter::add_fingerprint_loglik() {
  const std::size_t n = map_->size();
  for (std::size_t c = 0; c < n; ++c) loglik_[c] *= cfg_.fingerprint_model_weight;
  const double s2 = cfg_.fingerprint_sigma_m * cfg_.fingerprint_sigma_m;
  const double norm = 1.0 / (2.0 * std::numbers::pi * s2);
  const double eps = std::clamp(cfg_.fingerprint_eps, 1e-9, 1.0);
  const double uniform = eps / (static_cast<double>(n) * map_->cell_m() * map_->cell_m());
  for (std::size_t c = 0; c < n; ++c) {
    if (mass_[c] <= 0.0) continue;
    const int level = map_->cell(c).level;
    double sum = 0.0;
    for (int k = 0; k < neighbour_count_; ++k) {
      const auto& nb = neighbours_[static_cast<std::size_t>(k)];
      if (multi_level_ && nb.level != level) continue;
      const double dx = cx_[c] - nb.pos.x;
      const double dy = cy_[c] - nb.pos.y;
      sum += nb.weight * std::exp(-0.5 * (dx * dx + dy * dy) / s2);
    }
    loglik_[c] += cfg_.fingerprint_weight * std::log(uniform + (1.0 - eps) * norm * sum);
  }
}

/// The scan's floor classification as a likelihood on each cell's level.
void GridFilter::add_floor_vote_loglik() {
  const std::size_t n = map_->size();
  for (std::size_t c = 0; c < n; ++c) {
    if (mass_[c] > 0.0) {
      loglik_[c] += cfg_.floor_prior_weight * std::log(floor_p_[static_cast<std::size_t>(map_->cell(c).level)]);
    }
  }
}

}  // namespace ipe
