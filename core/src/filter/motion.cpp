// Motion: a step moves every cell's mass along its heading distribution; the magnetometer reweights it.

#include <algorithm>
#include <bit>
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
  const auto row = static_cast<std::size_t>(bins_);

  // Heading noise: a 3-tap kernel over the bins with the variance of heading_sigma per step. The bins are
  // offsets from the yaw, so the gyro turn itself needs no interpolation.
  const double bin_rad = kTwoPi / bins_;
  const double sigma = deg2rad(cfg_.heading_sigma_deg);
  const double a = std::clamp(0.5 * (sigma / bin_rad) * (sigma / bin_rad), 0.0, 1.0 / 3.0);
  for (const std::uint32_t c : active_) {
    if (mass_[c] <= 0.0) continue;
    float* hc = &h_[c * row];
    for (int k = 0; k < bins_; ++k) {
      const int lo = k == 0 ? bins_ - 1 : k - 1, hi = k + 1 == bins_ ? 0 : k + 1;
      row_[static_cast<std::size_t>(k)] = (1.0 - 2.0 * a) * hc[k] + a * (hc[lo] + hc[hi]);
    }
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(row_[i]);
  }
  const std::size_t n = map_->size();
  const std::size_t blocks = sparse_ ? map_->block_count() : 0;
  for (std::size_t b = 0; b < blocks; ++b) {
    if (!(bg_mass_[b] > 0.0)) continue;
    float* hb = &bg_h_[b * row];
    for (int k = 0; k < bins_; ++k) {
      const int lo = k == 0 ? bins_ - 1 : k - 1, hi = k + 1 == bins_ ? 0 : k + 1;
      row_[static_cast<std::size_t>(k)] = (1.0 - 2.0 * a) * hb[k] + a * (hb[lo] + hb[hi]);
    }
    for (std::size_t i = 0; i < row; ++i) hb[i] = static_cast<float>(row_[i]);
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

  for (const std::uint32_t c : active_) {
    acc_[c] = StepAcc{};
    touched_[c] = 0;
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    acc_[n + b] = StepAcc{};
    touched_[n + b] = 0;
  }
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
  const std::uint32_t* const active = active_.data();
  const std::size_t n_active = active_.size();
  const bool sparse = sparse_;
  const std::uint8_t* const cell_active = cell_active_.data();
  const std::uint8_t* const block_edge = block_edge_.data();
  for (std::size_t ai = 0; ai < n_active; ++ai) {
    const std::size_t c = active[ai];
    const double m = mass[c];
    if (m <= 0.0) continue;
    const double d1 = dev_mu_[c], d2 = dev_var_[c] + d1 * d1;
    const double b1 = baro_mu_[c], b2 = baro_var_[c] + b1 * b1;
    const GridMap::Cell& cell = map_->cell(c);
    const int cell_ix = cell.ix, cell_iy = cell.iy;
    std::uint64_t mask_lo = cell.mask[0], mask_hi = cell.mask[1];
    const std::int32_t* const around =
        map_->lattice_index(cell.level) + static_cast<std::ptrdiff_t>(cell_iy) * lattice_nx + cell_ix;
    if (sparse && block_edge[map_->block_of(c)] != 0) {
      // Next to the background: a move into a background block is not made, as if a wall stood there.
      // Only mass under the active floor can be here (refresh_active_set keeps a ring of active blocks
      // around every cell above it).
      for (std::uint64_t* word : {&mask_lo, &mask_hi}) {
        const int base = word == &mask_lo ? 0 : 64;
        for (std::uint64_t bits = *word; bits != 0; bits &= bits - 1) {
          const int b = base + std::countr_zero(bits);
          const int dx = b % side - radius, dy = b / side - radius;
          const std::int32_t t = around[static_cast<std::ptrdiff_t>(dy) * lattice_nx + dx];
          if (cell_active[static_cast<std::size_t>(t)] == 0) *word &= ~(std::uint64_t{1} << (b - base));
        }
      }
    }
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
  if (sparse_) move_background();
  step_motion_ = w_level > 0.0 ? move_level * (1.0 / w_level) : w_all > 0.0 ? move_all * (1.0 / w_all) : Vec2{};

  for (const std::uint32_t c : active_) {
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
  if (sparse_) settle_background();
  if (multi_level_) change_levels(cfg_.level_change_prob_step);
  normalize();
  maybe_teleport(step.t);
  update_heading(cloud_.level);
  update_estimate(step.t);
}

/// The background's step. A block's mass moves as one, by the mean move of its heading row: within the block
/// its centroid moves, into another background block it goes whole, into an active block it goes to the cell
/// there. A move through a wall or off the map ends it, as it ends a cell's.
void GridFilter::move_background() {
  const std::size_t n = map_->size();
  const auto row = static_cast<std::size_t>(bins_);
  const int radius = map_->radius();
  for (std::size_t b = 0; b < bg_mass_.size(); ++b) {
    const double m = bg_mass_[b];
    if (!(m > 0.0)) continue;
    const float* hb = &bg_h_[b * row];
    double mx = 0.0, my = 0.0;
    for (std::size_t k = 0; k < row; ++k) {
      mx += hb[k] * move_x_[k * 3 + 1];
      my += hb[k] * move_y_[k * 3 + 1];
    }
    const int level = map_->block_level(b);
    const Vec2 to{bg_x_[b] + mx, bg_y_[b] + my};
    const std::int32_t cell_to = map_->locate(to, level);
    bool open = cell_to >= 0;
    const std::int32_t cell_from = map_->locate({bg_x_[b], bg_y_[b]}, level);
    if (open && cell_from >= 0 && cell_from != cell_to) {
      const GridMap::Cell& from = map_->cell(static_cast<std::size_t>(cell_from));
      const GridMap::Cell& target = map_->cell(static_cast<std::size_t>(cell_to));
      const int dx = target.ix - from.ix, dy = target.iy - from.iy;
      open = std::abs(dx) <= radius && std::abs(dy) <= radius && GridMap::test(from, map_->bit(dx, dy));
    }
    if (!open) {
      bg_mass_[b] = 0.0;
      continue;
    }
    const auto ct = static_cast<std::size_t>(cell_to);
    const std::size_t block_to = map_->block_of(ct);
    if (block_to == b) {
      bg_x_[b] = to.x;
      bg_y_[b] = to.y;
      continue;
    }
    const std::size_t t = cell_active_[ct] != 0 ? ct : n + block_to;
    float* hn = &h_next_[t * row];
    if (touched_[t] == 0) {
      touched_[t] = 1;
      std::fill_n(hn, row, 0.0F);
    }
    for (std::size_t k = 0; k < row; ++k) hn[k] += static_cast<float>(m * hb[k]);
    StepAcc& in = acc_[t];
    in.mass += m;
    in.x += m * to.x;
    in.y += m * to.y;
    in.d1 += m * bg_dev_mu_[b];
    in.d2 += m * (bg_dev_var_[b] + bg_dev_mu_[b] * bg_dev_mu_[b]);
    in.b1 += m * bg_baro_mu_[b];
    in.b2 += m * (bg_baro_var_[b] + bg_baro_mu_[b] * bg_baro_mu_[b]);
    bg_mass_[b] = 0.0;
  }
}

/// What moved into a background block joins the mass it holds, moment-matched.
void GridFilter::settle_background() {
  const std::size_t n = map_->size();
  const auto row = static_cast<std::size_t>(bins_);
  for (std::size_t b = 0; b < bg_mass_.size(); ++b) {
    const StepAcc& in = acc_[n + b];
    if (touched_[n + b] == 0 || !(in.mass > 0.0)) continue;
    const double m0 = bg_mass_[b], m = m0 + in.mass;
    bg_x_[b] = (m0 * bg_x_[b] + in.x) / m;
    bg_y_[b] = (m0 * bg_y_[b] + in.y) / m;
    const double d1 = m0 * bg_dev_mu_[b] + in.d1;
    const double d2 = m0 * (bg_dev_var_[b] + bg_dev_mu_[b] * bg_dev_mu_[b]) + in.d2;
    const double b1 = m0 * bg_baro_mu_[b] + in.b1;
    const double b2 = m0 * (bg_baro_var_[b] + bg_baro_mu_[b] * bg_baro_mu_[b]) + in.b2;
    bg_dev_mu_[b] = d1 / m;
    bg_dev_var_[b] = cfg_.estimate_rssi_offset ? std::max(d2 / m - bg_dev_mu_[b] * bg_dev_mu_[b], kMinVar) : 0.0;
    bg_baro_mu_[b] = b1 / m;
    bg_baro_var_[b] = std::max(b2 / m - bg_baro_mu_[b] * bg_baro_mu_[b], kMinVar);
    const float* hn = &h_next_[(n + b) * row];
    float* hb = &bg_h_[b * row];
    double sum = 0.0;
    for (std::size_t i = 0; i < row; ++i) {
      row_[i] = m0 * hb[i] + hn[i];
      sum += row_[i];
    }
    for (std::size_t i = 0; i < row; ++i) {
      hb[i] = sum > 0.0 ? static_cast<float>(row_[i] / sum) : static_cast<float>(1.0 / bins_);
    }
    bg_mass_[b] = m;
  }
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
  for (const std::uint32_t c : active_) {
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
  if (sparse_) {
    // A background block's heading row is updated as a cell's.
    for (std::size_t b = 0; b < bg_mass_.size(); ++b) {
      if (!(bg_mass_[b] > 0.0)) continue;
      float* hb = &bg_h_[b * row];
      double sum = 0.0;
      for (std::size_t i = 0; i < row; ++i) sum += hb[i] * bin_lik_[i];
      if (sum <= 0.0) {
        bg_mass_[b] = 0.0;
        continue;
      }
      const double inv = 1.0 / sum;
      for (std::size_t i = 0; i < row; ++i) hb[i] = static_cast<float>(hb[i] * bin_lik_[i] * inv);
      bg_mass_[b] *= sum;
    }
  }
  normalize();
  maybe_teleport(t);
  update_heading(cloud_.level);
  update_estimate(t);
}

void GridFilter::update_heading(int level) {
  std::fill(bin_lik_.begin(), bin_lik_.end(), 0.0);
  for (const std::uint32_t c : active_on(level)) {
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
