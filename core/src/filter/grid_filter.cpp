// The grid filter: a Bayes filter over the cells of a GridMap (docs/design.md).
//
// State per cell: mass, mass centroid, device RSSI offset N(mu, var) and barometer offset N(mu, var).
// State per (cell, heading bin): the conditional distribution of the heading within the cell. Radio and
// barometer updates touch only the per-cell state; steps and the magnetometer touch all.
//
// This file holds the state and what every update ends with. The updates are next to it: motion.cpp (steps
// and heading), radio.cpp and fingerprint.cpp (a scan's log-likelihood terms), pressure.cpp (barometer and
// floor changes), recovery.cpp, estimate.cpp and active_set.cpp (which cells an update visits).

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

GridFilter::GridFilter(std::shared_ptr<const GridMap> map, GridFilterConfig cfg, RssiModelConfig rssi_cfg)
    : map_(std::move(map)),
      cfg_(cfg),
      rssi_model_(map_->venue(), rssi_cfg),
      bins_(std::max(cfg.heading_bins, 4)),
      multi_level_(map_->venue().multi_level()),
      integrity_(map_->venue().beacons().size(),
                 {kIntegrityThresholdDb, kIntegrityAlpha, kIntegrityMinSamples, kIntegrityMaxSigmaM}),
      floor_classifier_(map_->venue(), {kFloorPriorK, kFloorPriorEps}),
      fingerprint_map_(map_->venue(), {cfg.fingerprint_k, cfg.fingerprint_strongest}),
      level_selector_(cfg.level_smoothing_s, cfg.level_hysteresis) {
  const std::size_t n = map_->size();
  const std::size_t states = n * static_cast<std::size_t>(bins_);

  mass_.assign(n, 0.0);
  cx_.assign(n, 0.0);
  cy_.assign(n, 0.0);
  dev_mu_.assign(n, 0.0);
  dev_var_.assign(n, 0.0);
  baro_mu_.assign(n, 0.0);
  baro_var_.assign(n, 0.0);
  loglik_.assign(n, 0.0);
  h_.assign(states, 0.0F);
  sparse_ = cfg_.active_floor > 0.0;
  // With an active floor a step also moves mass into background blocks: block b is the cell n + b of the
  // step's scratch.
  const std::size_t blocks = map_->block_count();
  const std::size_t targets = n + (sparse_ ? blocks : 0);
  h_next_.assign(targets * static_cast<std::size_t>(bins_), 0.0F);
  touched_.assign(targets, 0);
  acc_.assign(targets, StepAcc{});
  move_x_.assign(static_cast<std::size_t>(bins_) * 3, 0.0);
  move_y_.assign(move_x_.size(), 0.0);
  move_offset_.assign(move_x_.size(), -1);
  const auto offsets = static_cast<std::size_t>((2 * map_->radius() + 1) * (2 * map_->radius() + 1));
  local_target_.assign(offsets, -1);
  local_w_.assign(offsets, 0.0);
  local_x_.assign(offsets, 0.0);
  local_y_.assign(offsets, 0.0);
  bin_lik_.assign(static_cast<std::size_t>(bins_), 0.0);
  row_.assign(static_cast<std::size_t>(bins_), 0.0);
  level_weight_.assign(static_cast<std::size_t>(std::max(map_->venue().level_count(), 1)), 0.0);
  level_dz_.assign(level_weight_.size(), 0.0);
  level_loss_.assign(level_weight_.size(), 0.0);
  active_floor_ = std::min(cfg_.active_floor, kMaxActiveFloor);
  active_.reserve(n);
  cell_active_.assign(n, 1);
  block_active_.assign(blocks, 1);
  block_edge_.assign(blocks, 0);
  block_mark_.assign(blocks, 0);
  bg_mass_.assign(blocks, 0.0);
  for (auto* v : {&bg_dev_mu_, &bg_dev_var_, &bg_baro_mu_, &bg_baro_var_}) v->assign(blocks, 0.0);
  block_loglik_.assign(blocks, 0.0);
  if (sparse_) {
    bg_h_.assign(blocks * static_cast<std::size_t>(bins_), static_cast<float>(1.0 / bins_));
    bg_x_.assign(blocks, 0.0);
    bg_y_.assign(blocks, 0.0);
    for (std::size_t b = 0; b < blocks; ++b) {
      bg_x_[b] = map_->block_centre(b).x;
      bg_y_[b] = map_->block_centre(b).y;
    }
  }
  floor_p_.assign(level_weight_.size(), 0.0);
  points_.reserve(n);
  scan_.reserve(map_->venue().beacons().size());
  const std::size_t conns = map_->connector_cells().size();
  for (auto* v : {&conn_mass_, &conn_x_, &conn_y_, &conn_d1_, &conn_d2_, &conn_b1_, &conn_b2_}) v->assign(conns, 0.0);
  conn_h_.assign(conns * static_cast<std::size_t>(bins_), 0.0F);
  map_estimate_.emplace(map_->venue(), n);
  initialize_uniform();
  update_estimate(0.0);
}

double GridFilter::bin_heading(int bin) const { return wrap_angle(yaw_ + kTwoPi * bin / bins_); }

double GridFilter::heading_prob(std::size_t cell, int bin) const { return h_[state(cell, bin)]; }

void GridFilter::initialize_uniform() {
  const std::size_t n = map_->size();
  const double u = 1.0 / static_cast<double>(n);
  std::fill(bg_mass_.begin(), bg_mass_.end(), 0.0);
  activate_all();
  for (std::size_t c = 0; c < n; ++c) reset_cell_to_prior(c, u);
}

void GridFilter::reset_cell_to_prior(std::size_t c, double mass) {
  mass_[c] = mass;
  const Vec2 p = map_->centre(c);
  cx_[c] = p.x;
  cy_[c] = p.y;
  dev_mu_[c] = 0.0;
  dev_var_[c] = prior_rssi_offset_var();
  baro_mu_[c] = prior_baro_offset(map_->cell(c).level);
  baro_var_[c] = prior_baro_offset_var();
  const auto v = static_cast<float>(1.0 / bins_);
  for (int k = 0; k < bins_; ++k) h_[state(c, k)] = v;
}

void GridFilter::set_point(Vec2 p, int level, int bin) {
  const std::int32_t target = map_->locate(p, level);
  if (target < 0) return;
  std::fill(bg_mass_.begin(), bg_mass_.end(), 0.0);
  activate_all();
  for (std::size_t c = 0; c < map_->size(); ++c) mass_[c] = 0.0;
  const auto c = static_cast<std::size_t>(target);
  mass_[c] = 1.0;
  cx_[c] = p.x;
  cy_[c] = p.y;
  for (int k = 0; k < bins_; ++k) h_[state(c, k)] = k == bin ? 1.0F : 0.0F;
  refresh_active_set();
  update_heading(level);
  update_estimate(now_);
}

void GridFilter::apply_cell_loglik(double t) {
  // Cells and background blocks share the reference: a block's mass stands for cells.
  const std::size_t blocks = sparse_ ? map_->block_count() : 0;
  double max_ll = -std::numeric_limits<double>::infinity();
  for (const std::uint32_t c : active_) {
    if (mass_[c] > 0.0) max_ll = std::max(max_ll, loglik_[c]);
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    if (bg_mass_[b] > 0.0) max_ll = std::max(max_ll, block_loglik_[b]);
  }
  if (std::isfinite(max_ll)) {
    for (const std::uint32_t c : active_) {
      if (mass_[c] > 0.0) mass_[c] *= std::exp(loglik_[c] - max_ll);
    }
    for (std::size_t b = 0; b < blocks; ++b) {
      if (bg_mass_[b] > 0.0) bg_mass_[b] *= std::exp(block_loglik_[b] - max_ll);
    }
  }
  normalize();
  maybe_teleport(t);
  update_estimate(t);
}

void GridFilter::normalize() {
  double sum = 0.0;
  for (const std::uint32_t c : active_) sum += mass_[c];
  if (sparse_) {
    for (const double m : bg_mass_) sum += m;
  }
  if (!(sum > 0.0) || !std::isfinite(sum)) {
    // Every hypothesis died (a door missing from the map, say): start again from the prior.
    initialize_uniform();
    return;
  }
  const double inv = 1.0 / sum;
  for (const std::uint32_t c : active_) mass_[c] *= inv;
  if (sparse_) {
    for (double& m : bg_mass_) m *= inv;
    refresh_active_set();
  }
}

}  // namespace ipe
