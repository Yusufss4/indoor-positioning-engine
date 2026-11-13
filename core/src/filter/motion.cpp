// Motion: a step moves every cell's mass along its heading distribution; the magnetometer reweights it.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

void GridFilter::on_step(const StepEvent& step) {
  now_ = step.t;
  yaw_ = wrap_angle(yaw_ + step.heading_delta);
  const std::size_t n = map_->size();
  const auto row = static_cast<std::size_t>(bins_);

  // Heading noise: a 3-tap kernel over the bins with the variance of heading_sigma per step. The bins are
  // offsets from the yaw, so the gyro turn itself needs no interpolation.
  const double bin_rad = kTwoPi / bins_;
  const double sigma = deg2rad(cfg_.heading_sigma_deg);
  const double a = std::clamp(0.5 * (sigma / bin_rad) * (sigma / bin_rad), 0.0, 1.0 / 3.0);
  for (std::size_t c = 0; c < n; ++c) {
    if (mass_[c] <= 0.0) continue;
    float* hc = &h_[c * row];
    for (int k = 0; k < bins_; ++k) {
      const int lo = k == 0 ? bins_ - 1 : k - 1, hi = k + 1 == bins_ ? 0 : k + 1;
      row_[static_cast<std::size_t>(k)] = (1.0 - 2.0 * a) * hc[k] + a * (hc[lo] + hc[hi]);
    }
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(row_[i]);
  }

  // Moves per (heading, node): along-track 3-point Gauss-Hermite nodes L +- sqrt(3) sigma, weighted
  // 1/6, 2/3, 1/6, which match the mean and variance of the length noise. In metres and in lattice units.
  constexpr double kNode = 1.7320508075688772;
  constexpr std::array<double, 3> kNodes{-kNode, 0.0, kNode};
  constexpr std::array<double, 3> kWeights{1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0};
  const double inv_cell = 1.0 / map_->cell_m();
  const double l = step.length;
  const double sl = std::hypot(cfg_.step_length_sigma, cfg_.step_length_rel_sigma * l);
  for (int k = 0; k < bins_; ++k) {
    const double th = yaw_ + bin_rad * k;
    const double ck = std::cos(th), sk = std::sin(th);
    for (std::size_t j = 0; j < 3; ++j) {
      const double len = std::max(0.0, l + kNodes[j] * sl);
      const std::size_t i = static_cast<std::size_t>(k) * 3 + j;
      move_x_[i] = len * ck;
      move_y_[i] = len * sk;
    }
  }

  std::fill(acc_.begin(), acc_.end(), StepAcc{});
  std::fill(touched_.begin(), touched_.end(), std::uint8_t{0});
  const int radius = map_->radius();
  const int side = 2 * radius + 1;
  const std::ptrdiff_t lattice_nx = map_->lattice_nx();
  const int shown_level = estimate_.level;
  const Vec2 origin = map_->lattice_centre(0, 0) - Vec2{0.5 * map_->cell_m(), 0.5 * map_->cell_m()};
  Vec2 move_level{}, move_all{};
  double w_level = 0.0, w_all = 0.0;
  // The scatter runs for every (cell, heading, node). It reads the members through local pointers and
  // keeps the cell's lattice position and mask in locals: the byte and float stores below could alias the
  // members, so the compiler would otherwise reload them on every iteration.
  const double* const mass = mass_.data();
  const float* const h = h_.data();
  float* const h_next = h_next_.data();
  std::uint8_t* const touched = touched_.data();
  const double* const move_x = move_x_.data();
  const double* const move_y = move_y_.data();
  std::int32_t* const local_target = local_target_.data();
  double* const local_w = local_w_.data();
  double* const local_x = local_x_.data();
  double* const local_y = local_y_.data();
  StepAcc* const acc = acc_.data();
  int* const move_offset = move_offset_.data();
  std::array<std::uint16_t, (2 * GridMap::kMaxRadius + 1) * (2 * GridMap::kMaxRadius + 1)> used{};
  for (std::size_t c = 0; c < n; ++c) {
    const double m = mass[c];
    if (m <= 0.0) continue;
    const double d1 = dev_mu_[c], d2 = dev_var_[c] + d1 * d1;
    const double b1 = baro_mu_[c], b2 = baro_var_[c] + b1 * b1;
    const GridMap::Cell& cell = map_->cell(c);
    const int cell_ix = cell.ix, cell_iy = cell.iy;
    const std::uint64_t mask_lo = cell.mask[0], mask_hi = cell.mask[1];
    const std::int32_t* const around =
        map_->lattice_index(cell.level) + static_cast<std::ptrdiff_t>(cell_iy) * lattice_nx + cell_ix;
    const double bx = cx_[c], by = cy_[c];
    const double ux = (bx - origin.x) * inv_cell, uy = (by - origin.y) * inv_cell;
    // Per target offset, the mass and its move summed over the cell's headings and nodes; the per-cell
    // accumulators are written once per target. Consecutive moves often land on the same offset, so its sums
    // stay in registers until another offset comes (the additions keep their order).
    std::size_t n_used = 0;
    int slot = -1;
    double run_w = 0.0, run_x = 0.0, run_y = 0.0;
    float* hn = nullptr;
    const float* const hc = h + c * row;
    // The lattice offset of every move from this cell, or -1 beyond the reach (a loop of its own, without
    // branches, so that the compiler vectorises it).
    for (std::size_t i = 0; i < row * 3; ++i) {
      const int dx = floor_int(ux + move_x[i] * inv_cell) - cell_ix;
      const int dy = floor_int(uy + move_y[i] * inv_cell) - cell_iy;
      const bool inside = dx >= -radius && dx <= radius && dy >= -radius && dy <= radius;
      move_offset[i] = inside ? (dy + radius) * side + dx + radius : -1;
    }
    for (std::size_t sk = 0; sk < row; ++sk) {
      const double p = m * hc[sk];
      if (p <= 0.0) continue;
      for (std::size_t j = 0; j < 3; ++j) {
        const double mx = move_x[sk * 3 + j], my = move_y[sk * 3 + j];
        const int b = move_offset[sk * 3 + j];
        if (b < 0) continue;
        if ((((b >> 6) != 0 ? mask_hi : mask_lo) >> (b & 63) & 1U) == 0) continue;
        if (b != slot) {
          if (slot >= 0) {
            local_w[slot] = run_w;
            local_x[slot] = run_x;
            local_y[slot] = run_y;
          }
          slot = b;
          if (local_target[b] < 0) {
            const int dx = b % side - radius, dy = b / side - radius;
            const std::int32_t t = around[static_cast<std::ptrdiff_t>(dy) * lattice_nx + dx];
            local_target[b] = t;
            used[n_used++] = static_cast<std::uint16_t>(b);
            run_w = run_x = run_y = 0.0;
            const auto ut = static_cast<std::size_t>(t);
            if (touched[ut] == 0) {
              // First mass into this cell: its new row starts empty (the cell's own mass moves too).
              touched[ut] = 1;
              std::fill_n(h_next + ut * row, row, 0.0F);
            }
          } else {
            run_w = local_w[b];
            run_x = local_x[b];
            run_y = local_y[b];
          }
          hn = h_next + static_cast<std::size_t>(local_target[b]) * row;
        }
        const double w = kWeights[j] * p;
        hn[sk] += static_cast<float>(w);
        run_w += w;
        run_x += w * mx;
        run_y += w * my;
      }
    }
    if (slot >= 0) {
      local_w[slot] = run_w;
      local_x[slot] = run_x;
      local_y[slot] = run_y;
    }
    for (std::size_t k = 0; k < n_used; ++k) {
      const std::uint16_t b = used[k];
      const auto t = static_cast<std::size_t>(local_target[b]);
      local_target[b] = -1;
      const double w = local_w[b];
      StepAcc& to = acc[t];
      to.mass += w;
      to.x += w * bx + local_x[b];
      to.y += w * by + local_y[b];
      to.d1 += w * d1;
      to.d2 += w * d2;
      to.b1 += w * b1;
      to.b2 += w * b2;
      move_all += Vec2{local_x[b], local_y[b]};
      w_all += w;
      if (cell.level == shown_level) {
        move_level += Vec2{local_x[b], local_y[b]};
        w_level += w;
      }
    }
  }
  step_motion_ = w_level > 0.0 ? move_level * (1.0 / w_level) : w_all > 0.0 ? move_all * (1.0 / w_all) : Vec2{};

  for (std::size_t c = 0; c < n; ++c) {
    const StepAcc& in = acc[c];
    const double m = in.mass;
    mass_[c] = m;
    if (m <= 0.0) {
      // Empty: whatever arrives later (recovery) brings its own centroid and heading.
      const Vec2 p = map_->centre(c);
      cx_[c] = p.x;
      cy_[c] = p.y;
      continue;
    }
    cx_[c] = in.x / m;
    cy_[c] = in.y / m;
    dev_mu_[c] = in.d1 / m;
    dev_var_[c] = cfg_.estimate_rssi_offset ? std::max(in.d2 / m - dev_mu_[c] * dev_mu_[c], kMinVar) : 0.0;
    baro_mu_[c] = in.b1 / m;
    baro_var_[c] = std::max(in.b2 / m - baro_mu_[c] * baro_mu_[c], kMinVar);
    const float* hn = &h_next_[c * row];
    double sum = 0.0;
    for (std::size_t i = 0; i < row; ++i) sum += hn[i];
    const double inv = sum > 0.0 ? 1.0 / sum : 0.0;
    float* hc = &h_[c * row];
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(hn[i] * inv);
  }
  if (multi_level_) change_levels(cfg_.level_change_prob_step);
  normalize();
  maybe_teleport(step.t);
  update_heading(cloud_.level);
  update_estimate(step.t);
}

void GridFilter::on_heading(double t, double heading, double sigma) {
  now_ = t;
  const double eps = cfg_.heading_outlier_prob;
  const double norm = 1.0 / (sigma * std::sqrt(kTwoPi));
  for (int k = 0; k < bins_; ++k) {
    const double z = wrap_angle(heading - (yaw_ + kTwoPi * k / bins_)) / sigma;
    bin_lik_[static_cast<std::size_t>(k)] = (1.0 - eps) * norm * std::exp(-0.5 * z * z) + eps / kTwoPi;
  }
  const auto row = static_cast<std::size_t>(bins_);
  for (std::size_t c = 0; c < map_->size(); ++c) {
    if (mass_[c] <= 0.0) continue;
    float* hc = &h_[c * row];
    double sum = 0.0;
    for (std::size_t i = 0; i < row; ++i) sum += hc[i] * bin_lik_[i];
    if (sum <= 0.0) {
      mass_[c] = 0.0;
      continue;
    }
    const double inv = 1.0 / sum;
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(hc[i] * bin_lik_[i] * inv);
    mass_[c] *= sum;
  }
  normalize();
  maybe_teleport(t);
  update_heading(cloud_.level);
  update_estimate(t);
}

void GridFilter::update_heading(int level) {
  const std::size_t begin = multi_level_ ? map_->level_begin(level) : 0;
  const std::size_t end = multi_level_ ? map_->level_begin(level + 1) : map_->size();
  std::fill(bin_lik_.begin(), bin_lik_.end(), 0.0);
  for (std::size_t c = begin; c < end; ++c) {
    if (mass_[c] <= 0.0) continue;
    for (int k = 0; k < bins_; ++k) bin_lik_[static_cast<std::size_t>(k)] += mass_[c] * h_[state(c, k)];
  }
  double sx = 0.0, sy = 0.0;
  for (int k = 0; k < bins_; ++k) {
    const double th = yaw_ + kTwoPi * k / bins_;
    sx += bin_lik_[static_cast<std::size_t>(k)] * std::cos(th);
    sy += bin_lik_[static_cast<std::size_t>(k)] * std::sin(th);
  }
  heading_ = std::atan2(sy, sx);
}

}  // namespace ipe
