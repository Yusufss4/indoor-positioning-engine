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
  const std::size_t blocks = sparse_ ? map_->block_count() : 0;
  for (const std::uint32_t c : active_) loglik_[c] *= cfg_.fingerprint_model_weight;
  for (std::size_t b = 0; b < blocks; ++b) block_loglik_[b] *= cfg_.fingerprint_model_weight;
  const double s2 = cfg_.fingerprint_sigma_m * cfg_.fingerprint_sigma_m;
  const double norm = 1.0 / (2.0 * std::numbers::pi * s2);
  const double eps = std::clamp(cfg_.fingerprint_eps, 1e-9, 1.0);
  const double uniform = eps / (static_cast<double>(n) * map_->cell_m() * map_->cell_m());
  const auto term = [&](double x, double y, int level) {
    double sum = 0.0;
    for (int k = 0; k < neighbour_count_; ++k) {
      const auto& nb = neighbours_[static_cast<std::size_t>(k)];
      if (multi_level_ && nb.level != level) continue;
      const double dx = x - nb.pos.x;
      const double dy = y - nb.pos.y;
      sum += nb.weight * std::exp(-0.5 * (dx * dx + dy * dy) / s2);
    }
    return cfg_.fingerprint_weight * std::log(uniform + (1.0 - eps) * norm * sum);
  };
  for (const std::uint32_t c : active_) {
    if (mass_[c] > 0.0) loglik_[c] += term(cx_[c], cy_[c], map_->cell(c).level);
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    if (!(bg_mass_[b] > 0.0)) continue;
    const Vec2 p = bg_pos(b);
    block_loglik_[b] += term(p.x, p.y, map_->block_level(b));
  }
}

/// The scan's floor classification as a likelihood on each cell's level.
void GridFilter::add_floor_vote_loglik() {
  for (const std::uint32_t c : active_) {
    if (mass_[c] > 0.0) {
      loglik_[c] += cfg_.floor_prior_weight * std::log(floor_p_[static_cast<std::size_t>(map_->cell(c).level)]);
    }
  }
  if (!sparse_) return;
  for (std::size_t b = 0; b < map_->block_count(); ++b) {
    block_loglik_[b] += cfg_.floor_prior_weight * std::log(floor_p_[static_cast<std::size_t>(map_->block_level(b))]);
  }
}

}  // namespace ipe
