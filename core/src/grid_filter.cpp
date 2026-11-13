// The grid filter: a Bayes filter over the cells of a GridMap (docs/grid.md).
//
// State per cell: mass, mass centroid, device RSSI offset N(mu, var) and barometer offset N(mu, var).
// State per (cell, scale, heading bin): the conditional distribution of step scale and heading within the
// cell. Radio and barometer updates touch only the per-cell state; steps and the magnetometer touch all.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "ipe/grid_filter.hpp"

namespace ipe {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kMinVar = 1e-6;
/// Cells under this share of the level's heaviest cell are left out of the map-consistent estimate: every
/// cell holds some recovery mass, and that mass is not a group of hypotheses.
constexpr double kPointFloorRel = 1e-3;

/// std::floor to int, for |v| < 2^31.
int floor_int(double v) {
  const int i = static_cast<int>(v);
  return i - (v < static_cast<double>(i));
}

/// Mixes N(mu2, var2) into N(mu, var) with weight f (moment matching).
void mix_gaussian(double& mu, double& var, double mu2, double var2, double f) {
  const double m = (1.0 - f) * mu + f * mu2;
  const double e2 = (1.0 - f) * (var + mu * mu) + f * (var2 + mu2 * mu2);
  mu = m;
  var = std::max(e2 - m * m, kMinVar);
}

/// Robust Gaussian/uniform likelihood of residual `r` with predictive variance `s2`; `resp` gets the inlier
/// responsibility.
double mixture(double r, double s2, double outlier_prob, double outlier_density, double& resp) {
  const double g = (1.0 - outlier_prob) * std::exp(-0.5 * r * r / s2) / std::sqrt(kTwoPi * s2);
  const double lik = g + outlier_prob * outlier_density;
  resp = lik > 0.0 ? g / lik : 0.0;
  return lik;
}

/// Assumed-density update of N(mu, var) from a reading z = x + mu + noise(noise_var), with residual
/// `r` = z - x - mu and inlier responsibility `resp`.
void adf_update(double& mu, double& var, double r, double noise_var, double resp) {
  const double s2 = noise_var + var;
  const double gain = var / s2;
  const double shift = gain * r;
  const double post_var = var * (1.0 - gain);
  mu += resp * shift;
  var = std::max(resp * post_var + (1.0 - resp) * var + resp * (1.0 - resp) * shift * shift, kMinVar);
}

}  // namespace

GridFilter::GridFilter(std::shared_ptr<const GridMap> map, GridFilterConfig cfg, RssiModelConfig rssi_cfg,
                       BaroConfig baro_cfg)
    : map_(std::move(map)),
      cfg_(cfg),
      rssi_model_(map_->venue(), rssi_cfg),
      bins_(std::max(cfg.heading_bins, 4)),
      scales_(std::clamp(cfg.step_scale_bins, 1, 15) | 1),
      multi_level_(map_->venue().multi_level()),
      integrity_(map_->venue().beacons().size(), {cfg.integrity_threshold_db, cfg.integrity_alpha,
                                                  cfg.integrity_min_samples, cfg.integrity_max_sigma_m}),
      floor_classifier_(map_->venue(), {cfg.floor_prior_k, cfg.floor_prior_eps}),
      fingerprint_map_(map_->venue(), {cfg.fingerprint_k, cfg.fingerprint_strongest}),
      level_selector_(cfg.level_smoothing_s, cfg.level_hysteresis),
      altimeter_(baro_cfg) {
  const std::size_t n = map_->size();
  const std::size_t states = n * static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_);
  double prior_sum = 0.0;
  for (int s = 0; s < scales_; ++s) {
    const double v = 1.0 + (s - (scales_ - 1) / 2) * cfg_.step_scale_spread;
    const double z = scales_ > 1 ? (v - 1.0) / cfg_.step_scale_sigma : 0.0;
    scale_value_.push_back(v);
    scale_prior_.push_back(std::exp(-0.5 * z * z));
    prior_sum += scale_prior_.back();
  }
  for (double& p : scale_prior_) p /= prior_sum;

  mass_.assign(n, 0.0);
  cx_.assign(n, 0.0);
  cy_.assign(n, 0.0);
  dev_mu_.assign(n, 0.0);
  dev_var_.assign(n, 0.0);
  baro_mu_.assign(n, 0.0);
  baro_var_.assign(n, 0.0);
  loglik_.assign(n, 0.0);
  h_.assign(states, 0.0F);
  h_next_.assign(states, 0.0F);
  touched_.assign(n, 0);
  acc_.assign(n, StepAcc{});
  move_x_.assign(static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_) * 3, 0.0);
  move_y_.assign(move_x_.size(), 0.0);
  move_offset_.assign(move_x_.size(), -1);
  const auto offsets = static_cast<std::size_t>((2 * map_->radius() + 1) * (2 * map_->radius() + 1));
  local_target_.assign(offsets, -1);
  local_w_.assign(offsets, 0.0);
  local_x_.assign(offsets, 0.0);
  local_y_.assign(offsets, 0.0);
  row_lik_.assign(static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_), 0.0);
  bin_lik_.assign(static_cast<std::size_t>(bins_), 0.0);
  row_.assign(static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_), 0.0);
  level_weight_.assign(static_cast<std::size_t>(std::max(map_->venue().level_count(), 1)), 0.0);
  floor_p_.assign(level_weight_.size(), 0.0);
  points_.reserve(n);
  scan_.reserve(map_->venue().beacons().size());
  const std::size_t conns = map_->connector_cells().size();
  for (auto* v : {&conn_mass_, &conn_x_, &conn_y_, &conn_d1_, &conn_d2_, &conn_b1_, &conn_b2_}) v->assign(conns, 0.0);
  conn_h_.assign(conns * static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_), 0.0F);
  if (cfg_.map_consistent_estimate) map_estimate_.emplace(map_->venue(), n);
  initialize_uniform();
  update_estimate(0.0);
}

double GridFilter::bin_heading(int bin) const { return wrap_angle(yaw_ + kTwoPi * bin / bins_); }

double GridFilter::heading_prob(std::size_t cell, int bin) const {
  double p = 0.0;
  for (int s = 0; s < scales_; ++s) p += h_[state(cell, s, bin)];
  return p;
}

void GridFilter::initialize_uniform() {
  const std::size_t n = map_->size();
  const double u = 1.0 / static_cast<double>(n);
  for (std::size_t c = 0; c < n; ++c) {
    mass_[c] = u;
    const Vec2 p = map_->centre(c);
    cx_[c] = p.x;
    cy_[c] = p.y;
    dev_mu_[c] = 0.0;
    dev_var_[c] = cfg_.estimate_rssi_offset ? cfg_.rssi_offset_sigma * cfg_.rssi_offset_sigma : 0.0;
    const int level = map_->cell(c).level;
    baro_mu_[c] = baro_ready_ ? altitude_ - elevation(level) : 0.0;
    baro_var_[c] = cfg_.baro_offset_sigma_m * cfg_.baro_offset_sigma_m;
    for (int s = 0; s < scales_; ++s) {
      const auto v = static_cast<float>(scale_prior_[static_cast<std::size_t>(s)] / bins_);
      for (int k = 0; k < bins_; ++k) h_[state(c, s, k)] = v;
    }
  }
}

void GridFilter::set_point(Vec2 p, int level, int bin) {
  const std::int32_t target = map_->locate(p, level);
  if (target < 0) return;
  for (std::size_t c = 0; c < map_->size(); ++c) mass_[c] = 0.0;
  const auto c = static_cast<std::size_t>(target);
  mass_[c] = 1.0;
  cx_[c] = p.x;
  cy_[c] = p.y;
  for (int s = 0; s < scales_; ++s) {
    for (int k = 0; k < bins_; ++k) {
      h_[state(c, s, k)] = k == bin ? static_cast<float>(scale_prior_[static_cast<std::size_t>(s)]) : 0.0F;
    }
  }
  update_heading(level);
  update_estimate(now_);
}

void GridFilter::on_step(const StepEvent& step) {
  now_ = step.t;
  yaw_ = wrap_angle(yaw_ + step.heading_delta);
  const std::size_t n = map_->size();
  const std::size_t row = static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_);
  const double floor = active_floor();
  const auto active = [&](std::size_t c) { return mass_[c] > 0.0 && mass_[c] >= floor; };

  // Heading noise: a 3-tap kernel over the bins with the variance of heading_sigma per step. The bins are
  // offsets from the yaw, so the gyro turn itself needs no interpolation.
  const double bin_rad = kTwoPi / bins_;
  const double sigma = deg2rad(cfg_.heading_sigma_deg);
  const double a = std::clamp(0.5 * (sigma / bin_rad) * (sigma / bin_rad), 0.0, 1.0 / 3.0);
  const double sw = scales_ > 1 ? std::clamp(cfg_.step_scale_switch, 0.0, 0.5) : 0.0;
  for (std::size_t c = 0; c < n; ++c) {
    if (!active(c)) continue;
    float* hc = &h_[c * row];
    for (int s = 0; s < scales_; ++s) {
      const float* src = hc + static_cast<std::size_t>(s) * static_cast<std::size_t>(bins_);
      double* dst = &row_[static_cast<std::size_t>(s) * static_cast<std::size_t>(bins_)];
      for (int k = 0; k < bins_; ++k) {
        const int lo = k == 0 ? bins_ - 1 : k - 1, hi = k + 1 == bins_ ? 0 : k + 1;
        dst[k] = (1.0 - 2.0 * a) * src[k] + a * (src[lo] + src[hi]);
      }
    }
    if (sw > 0.0) {
      // Scale switch to a neighbouring value, reflecting at the ends.
      for (int k = 0; k < bins_; ++k) {
        double prev = 0.0;
        for (int s = 0; s < scales_; ++s) {
          const std::size_t i =
              static_cast<std::size_t>(s) * static_cast<std::size_t>(bins_) + static_cast<std::size_t>(k);
          const double here = row_[i];
          const double next = s + 1 < scales_ ? row_[i + static_cast<std::size_t>(bins_)] : 0.0;
          const int neighbours = (s > 0) + (s + 1 < scales_);
          row_[i] = here * (1.0 - sw * neighbours) + sw * (prev + next);
          prev = here;
        }
      }
    }
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(row_[i]);
  }

  // Moves per (scale, heading, node): along-track 3-point Gauss-Hermite nodes L +- sqrt(3) sigma, weighted
  // 1/6, 2/3, 1/6, which match the mean and variance of the length noise. In metres and in lattice units.
  constexpr double kNode = 1.7320508075688772;
  constexpr std::array<double, 3> kNodes{-kNode, 0.0, kNode};
  constexpr std::array<double, 3> kWeights{1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0};
  const double inv_cell = 1.0 / map_->cell_m();
  for (int k = 0; k < bins_; ++k) {
    const double th = yaw_ + bin_rad * k;
    const double ck = std::cos(th), sk = std::sin(th);
    for (int s = 0; s < scales_; ++s) {
      const double l = step.length * scale_value_[static_cast<std::size_t>(s)];
      const double sl = std::hypot(cfg_.step_length_sigma, cfg_.step_length_rel_sigma * l);
      for (std::size_t j = 0; j < 3; ++j) {
        const double len = std::max(0.0, l + kNodes[j] * sl);
        const std::size_t i =
            (static_cast<std::size_t>(s) * static_cast<std::size_t>(bins_) + static_cast<std::size_t>(k)) * 3 + j;
        move_x_[i] = len * ck;
        move_y_[i] = len * sk;
      }
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
  // The scatter runs for every (cell, scale, heading, node). It reads the members through local pointers and
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
    if (!active(c)) {
      // Background: uniform recovery mass, which a step leaves uniform. It stays where it is.
      StepAcc& stay = acc[c];
      stay.mass += m;
      stay.x += m * cx_[c];
      stay.y += m * cy_[c];
      stay.d1 += m * d1;
      stay.d2 += m * d2;
      stay.b1 += m * b1;
      stay.b2 += m * b2;
      continue;
    }
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
              // First mass into this cell: its new row starts from the mass that stays in it, none for an
              // active cell (its own mass moves), all of it for a background cell (it stays put).
              touched[ut] = 1;
              float* const ht = h_next + ut * row;
              if (mass[ut] > 0.0 && !active(ut)) {
                for (std::size_t i = 0; i < row; ++i) ht[i] = static_cast<float>(mass[ut] * h[ut * row + i]);
              } else {
                std::fill_n(ht, row, 0.0F);
              }
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
    if (!touched_[c]) continue;  // a background cell nothing moved into keeps its row
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

double GridFilter::active_floor() const {
  if (cfg_.active_floor_rel <= 0.0) return 0.0;
  double max_mass = 0.0;
  for (double m : mass_) max_mass = std::max(max_mass, m);
  return cfg_.active_floor_rel * max_mass;
}

void GridFilter::on_heading(double t, double heading, double sigma) {
  now_ = t;
  const double eps = cfg_.heading_outlier_prob;
  const double norm = 1.0 / (sigma * std::sqrt(kTwoPi));
  for (int k = 0; k < bins_; ++k) {
    const double z = wrap_angle(heading - (yaw_ + kTwoPi * k / bins_)) / sigma;
    bin_lik_[static_cast<std::size_t>(k)] = (1.0 - eps) * norm * std::exp(-0.5 * z * z) + eps / kTwoPi;
  }
  const std::size_t row = static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_);
  const double floor = active_floor();
  double mean_lik = 0.0;
  for (double l : bin_lik_) mean_lik += l / bins_;
  for (std::size_t i = 0; i < row; ++i) row_lik_[i] = bin_lik_[i % static_cast<std::size_t>(bins_)];
  for (std::size_t c = 0; c < map_->size(); ++c) {
    if (mass_[c] <= 0.0) continue;
    if (mass_[c] < floor) {
      // Background mass is uniform over headings.
      mass_[c] *= mean_lik;
      continue;
    }
    float* hc = &h_[c * row];
    double sum = 0.0;
    for (std::size_t i = 0; i < row; ++i) sum += hc[i] * row_lik_[i];
    if (sum <= 0.0) {
      mass_[c] = 0.0;
      continue;
    }
    const double inv = 1.0 / sum;
    for (std::size_t i = 0; i < row; ++i) hc[i] = static_cast<float>(hc[i] * row_lik_[i] * inv);
    mass_[c] *= sum;
  }
  normalize();
  maybe_teleport(t);
  update_heading(cloud_.level);
  update_estimate(t);
}

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
  const RssiModelConfig& rc = rssi_model_.config();
  const double noise_var = rc.sigma_db * rc.sigma_db;
  const double outlier_density = 1.0 / rc.outlier_range_db;
  const double wall_db = map_->venue().wall_attenuation_db();
  const double walk2 = cfg_.rssi_offset_walk * cfg_.rssi_offset_walk;
  const std::size_t n = map_->size();
  for (std::size_t c = 0; c < n; ++c) loglik_[c] = 0.0;
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
  // The scan's nearest training scans as a kernel density over position and level, mixed with the uniform
  // prior over the cells. The model reads the same scan,
  // so it counts for less next to them.
  if (neighbour_count_ > 0) {
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
  // The scan's floor classification as a likelihood on each cell's level (before the recovery statistic,
  // as in the particle filter).
  if (floor_prior_active_) {
    for (std::size_t c = 0; c < n; ++c) {
      if (mass_[c] > 0.0) {
        loglik_[c] += cfg_.floor_prior_weight * std::log(floor_p_[static_cast<std::size_t>(map_->cell(c).level)]);
      }
    }
  }
  if (cfg_.recovery_max_fraction > 0.0) {
    // Predictive likelihood of the update under the current distribution, as augmented MCL averages it.
    double max_ll = -std::numeric_limits<double>::infinity();
    for (std::size_t c = 0; c < n; ++c) {
      if (mass_[c] > 0.0) max_ll = std::max(max_ll, loglik_[c]);
    }
    if (std::isfinite(max_ll)) {
      double avg = 0.0;
      for (std::size_t c = 0; c < n; ++c) {
        if (mass_[c] > 0.0) avg += mass_[c] * std::exp(loglik_[c] - max_ll);
      }
      const double w_avg = avg * std::exp(max_ll);
      if (w_slow_ == 0.0) w_slow_ = w_fast_ = w_avg;
      w_slow_ += cfg_.recovery_alpha_slow * (w_avg - w_slow_);
      w_fast_ += cfg_.recovery_alpha_fast * (w_avg - w_fast_);
    }
  }
  localized_ = true;
  apply_cell_loglik(t);
}

void GridFilter::on_pressure(double t, double hpa) {
  now_ = t;
  altitude_ = altimeter_.process(t, hpa);
  if (!multi_level_) return;
  const std::size_t n = map_->size();
  if (!baro_ready_) {
    for (std::size_t c = 0; c < n; ++c) {
      baro_mu_[c] = altitude_ - elevation(map_->cell(c).level);
      baro_var_[c] = cfg_.baro_offset_sigma_m * cfg_.baro_offset_sigma_m;
    }
    baro_ready_ = true;
    last_baro_update_t_ = t;
    return;
  }
  if (t - last_baro_update_t_ < cfg_.baro_update_interval_s) return;
  last_baro_update_t_ = t;

  change_levels(cfg_.level_change_prob_baro);
  const double noise_var = cfg_.baro_sigma_m * cfg_.baro_sigma_m;
  const double walk2 = cfg_.baro_offset_walk_m * cfg_.baro_offset_walk_m;
  for (std::size_t c = 0; c < n; ++c) {
    if (mass_[c] <= 0.0) continue;
    baro_var_[c] += walk2;
    const double res = altitude_ - baro_mu_[c] - elevation(map_->cell(c).level);
    double resp = 0.0;
    const double lik = mixture(res, noise_var + baro_var_[c], cfg_.baro_outlier_prob, 1.0 / 20.0, resp);
    loglik_[c] = std::log(lik);
    adf_update(baro_mu_[c], baro_var_[c], res, noise_var, resp);
  }
  apply_cell_loglik(t);
}

void GridFilter::apply_cell_loglik(double t) {
  const std::size_t n = map_->size();
  double max_ll = -std::numeric_limits<double>::infinity();
  for (std::size_t c = 0; c < n; ++c) {
    if (mass_[c] > 0.0) max_ll = std::max(max_ll, loglik_[c]);
  }
  if (std::isfinite(max_ll)) {
    for (std::size_t c = 0; c < n; ++c) {
      if (mass_[c] > 0.0) mass_[c] *= std::exp(loglik_[c] - max_ll);
    }
  }
  normalize();
  maybe_teleport(t);
  update_estimate(t);
}

void GridFilter::normalize() {
  double sum = 0.0;
  for (double m : mass_) sum += m;
  if (!(sum > 0.0) || !std::isfinite(sum)) {
    // Every hypothesis died (a door missing from the map, say): start again from the prior.
    initialize_uniform();
    return;
  }
  const double inv = 1.0 / sum;
  for (double& m : mass_) m *= inv;
}

void GridFilter::maybe_teleport(double t) {
  if (!teleport_started_) {
    teleport_started_ = true;
    last_teleport_t_ = t;
    return;
  }
  const double dt = t - last_teleport_t_;
  if (dt < cfg_.teleport_period_s) return;
  last_teleport_t_ = t;
  double rho = 1.0 - std::exp(-std::max(cfg_.teleport_rate_per_s, 0.0) * dt);
  if (cfg_.recovery_max_fraction > 0.0 && w_slow_ > 0.0) {
    rho += std::clamp(1.0 - w_fast_ / w_slow_, 0.0, cfg_.recovery_max_fraction);
  }
  rho = std::min(rho, 1.0);
  if (rho <= 0.0) return;
  const std::size_t n = map_->size();
  const double u = rho / static_cast<double>(n);
  const double dev_var0 = cfg_.estimate_rssi_offset ? cfg_.rssi_offset_sigma * cfg_.rssi_offset_sigma : 0.0;
  const double baro_var0 = cfg_.baro_offset_sigma_m * cfg_.baro_offset_sigma_m;
  const std::size_t row = static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_);
  for (std::size_t c = 0; c < n; ++c) {
    const double kept = (1.0 - rho) * mass_[c];
    const double m = kept + u;
    const double f = u / m;
    const Vec2 p = map_->centre(c);
    cx_[c] = (1.0 - f) * cx_[c] + f * p.x;
    cy_[c] = (1.0 - f) * cy_[c] + f * p.y;
    mix_gaussian(dev_mu_[c], dev_var_[c], 0.0, dev_var0, f);
    if (!cfg_.estimate_rssi_offset) dev_var_[c] = 0.0;
    const double b0 = baro_ready_ ? altitude_ - elevation(map_->cell(c).level) : 0.0;
    mix_gaussian(baro_mu_[c], baro_var_[c], b0, baro_var0, f);
    mass_[c] = m;
    if (f < 1e-9) continue;  // the heading row would not change in float
    float* hc = &h_[c * row];
    for (int s = 0; s < scales_; ++s) {
      const double prior = scale_prior_[static_cast<std::size_t>(s)] / bins_;
      for (int k = 0; k < bins_; ++k) {
        float& v = hc[static_cast<std::size_t>(s) * static_cast<std::size_t>(bins_) + static_cast<std::size_t>(k)];
        v = static_cast<float>((1.0 - f) * v + f * prior);
      }
    }
  }
}

void GridFilter::change_levels(double prob) {
  const auto conns = map_->connector_cells();
  if (conns.empty() || prob <= 0.0) return;
  const std::size_t row = static_cast<std::size_t>(scales_) * static_cast<std::size_t>(bins_);
  for (std::size_t i = 0; i < conns.size(); ++i) {
    const std::size_t c = conns[i];
    conn_mass_[i] = mass_[c];
    conn_x_[i] = cx_[c];
    conn_y_[i] = cy_[c];
    conn_d1_[i] = dev_mu_[c];
    conn_d2_[i] = dev_var_[c];
    conn_b1_[i] = baro_mu_[c];
    conn_b2_[i] = baro_var_[c];
    std::copy_n(&h_[c * row], row, &conn_h_[i * row]);
  }
  // Outflows first (they leave the moments of what stays unchanged), then inflows from the copies.
  for (std::size_t i = 0; i < conns.size(); ++i) mass_[conns[i]] -= prob * conn_mass_[i];
  for (std::size_t i = 0; i < conns.size(); ++i) {
    const GridMap::Cell& cell = map_->cell(conns[i]);
    const int targets = (cell.up >= 0) + (cell.down >= 0);
    const double out = prob * conn_mass_[i];
    if (out <= 0.0 || targets == 0) continue;
    for (const std::int32_t to : {cell.up, cell.down}) {
      if (to < 0) continue;
      const auto t = static_cast<std::size_t>(to);
      const double w = out / targets;
      const double m = std::max(mass_[t], 0.0) + w;
      const double f = w / m;
      cx_[t] = (1.0 - f) * cx_[t] + f * conn_x_[i];
      cy_[t] = (1.0 - f) * cy_[t] + f * conn_y_[i];
      mix_gaussian(dev_mu_[t], dev_var_[t], conn_d1_[i], conn_d2_[i], f);
      if (!cfg_.estimate_rssi_offset) dev_var_[t] = 0.0;
      mix_gaussian(baro_mu_[t], baro_var_[t], conn_b1_[i], conn_b2_[i], f);
      float* ht = &h_[t * row];
      const float* hs = &conn_h_[i * row];
      for (std::size_t k = 0; k < row; ++k) ht[k] = static_cast<float>((1.0 - f) * ht[k] + f * hs[k]);
      mass_[t] = m;
    }
  }
}

void GridFilter::update_heading(int level) {
  const std::size_t begin = multi_level_ ? map_->level_begin(level) : 0;
  const std::size_t end = multi_level_ ? map_->level_begin(level + 1) : map_->size();
  std::fill(bin_lik_.begin(), bin_lik_.end(), 0.0);
  for (std::size_t c = begin; c < end; ++c) {
    if (mass_[c] <= 0.0) continue;
    for (int s = 0; s < scales_; ++s) {
      for (int k = 0; k < bins_; ++k) bin_lik_[static_cast<std::size_t>(k)] += mass_[c] * h_[state(c, s, k)];
    }
  }
  double sx = 0.0, sy = 0.0;
  for (int k = 0; k < bins_; ++k) {
    const double th = yaw_ + kTwoPi * k / bins_;
    sx += bin_lik_[static_cast<std::size_t>(k)] * std::cos(th);
    sy += bin_lik_[static_cast<std::size_t>(k)] * std::sin(th);
  }
  heading_ = std::atan2(sy, sx);
}

void GridFilter::update_estimate(double t) {
  const std::size_t n = map_->size();
  int top = 0;
  if (multi_level_) {
    for (int l = 0; l < static_cast<int>(level_weight_.size()); ++l) {
      double w = 0.0;
      for (std::size_t c = map_->level_begin(l); c < map_->level_begin(l + 1); ++c) w += mass_[c];
      level_weight_[static_cast<std::size_t>(l)] = w;
    }
    top = static_cast<int>(std::max_element(level_weight_.begin(), level_weight_.end()) - level_weight_.begin());
  }
  const double within = map_->cell_m() * map_->cell_m() / 12.0;
  struct Moments {
    Vec2 mean;
    double cxx = 0.0, cxy = 0.0, cyy = 0.0, dev = 0.0, max_mass = 0.0;
  };
  const auto moments = [&](int level) {
    const std::size_t begin = multi_level_ ? map_->level_begin(level) : 0;
    const std::size_t end = multi_level_ ? map_->level_begin(level + 1) : n;
    Moments m;
    double w = 0.0, sx = 0.0, sy = 0.0;
    for (std::size_t c = begin; c < end; ++c) {
      w += mass_[c];
      sx += mass_[c] * cx_[c];
      sy += mass_[c] * cy_[c];
      m.dev += mass_[c] * dev_mu_[c];
      m.max_mass = std::max(m.max_mass, mass_[c]);
    }
    if (w <= 0.0) return m;
    m.mean = {sx / w, sy / w};
    m.dev /= w;
    for (std::size_t c = begin; c < end; ++c) {
      const double dx = cx_[c] - m.mean.x, dy = cy_[c] - m.mean.y;
      m.cxx += mass_[c] * dx * dx;
      m.cxy += mass_[c] * dx * dy;
      m.cyy += mass_[c] * dy * dy;
    }
    m.cxx = m.cxx / w + within;
    m.cxy /= w;
    m.cyy = m.cyy / w + within;
    return m;
  };
  const auto to_estimate = [&](const Moments& m, int level) {
    Estimate e;
    e.t = t;
    e.pos = m.mean;
    e.heading = heading_;
    e.cov_xx = m.cxx;
    e.cov_xy = m.cxy;
    e.cov_yy = m.cyy;
    e.level = level;
    e.z = multi_level_ ? elevation(level) : 0.0;
    e.valid = localized_;
    return e;
  };

  const Moments cloud = moments(top);
  mean_rssi_offset_ = cloud.dev;
  cloud_ = to_estimate(cloud, top);
  if (!localized_) level_selector_.reset();
  const int level = multi_level_ ? level_selector_.update(t, level_weight_) : 0;
  const Moments shown = level == top ? cloud : moments(level);
  estimate_ = to_estimate(shown, level);
  if (!map_estimate_ || !localized_) return;
  points_.clear();
  const std::size_t begin = multi_level_ ? map_->level_begin(level) : 0;
  const std::size_t end = multi_level_ ? map_->level_begin(level + 1) : n;
  const double floor = kPointFloorRel * shown.max_mass;
  for (std::size_t c = begin; c < end; ++c) {
    if (mass_[c] <= 0.0 || mass_[c] < floor) continue;
    WeightedPoint p;
    p.pos = {cx_[c], cy_[c]};
    p.weight = mass_[c];
    p.level = level;
    points_.push_back(p);
  }
  const Vec2 pos = map_estimate_->position(points_, shown.mean, level, multi_level_);
  const Vec2 off = shown.mean - pos;
  estimate_.pos = pos;
  estimate_.cov_xx += off.x * off.x;
  estimate_.cov_xy += off.x * off.y;
  estimate_.cov_yy += off.y * off.y;
}

}  // namespace ipe
