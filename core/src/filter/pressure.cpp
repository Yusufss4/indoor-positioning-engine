// Barometer: the altitude against each cell's level, and mass changing level inside stairwells and lifts.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

void GridFilter::on_pressure(double t, double hpa) {
  now_ = t;
  altitude_ = altimeter_.process(t, hpa);
  if (!multi_level_) return;
  const std::size_t n = map_->size();
  if (!baro_ready_) {
    for (std::size_t c = 0; c < n; ++c) {
      baro_mu_[c] = altitude_ - elevation(map_->cell(c).level);
      baro_var_[c] = prior_baro_offset_var();
    }
    for (std::size_t b = 0; b < bg_baro_mu_.size(); ++b) {
      bg_baro_mu_[b] = altitude_ - elevation(map_->block_level(b));
      bg_baro_var_[b] = prior_baro_offset_var();
    }
    baro_ready_ = true;
    last_baro_update_t_ = t;
    return;
  }
  if (t - last_baro_update_t_ < kBaroUpdateIntervalS) return;
  last_baro_update_t_ = t;

  change_levels(kLevelChangeProbBaro);
  const double noise_var = kBaroSigmaM * kBaroSigmaM;
  const double walk2 = kBaroOffsetWalkM * kBaroOffsetWalkM;
  for (const std::uint32_t c : active_) {
    if (mass_[c] <= 0.0) continue;
    baro_var_[c] += walk2;
    const double res = altitude_ - baro_mu_[c] - elevation(map_->cell(c).level);
    double resp = 0.0;
    const double lik = mixture(res, noise_var + baro_var_[c], kBaroOutlierProb, 1.0 / 20.0, resp);
    loglik_[c] = std::log(lik);
    adf_update(baro_mu_[c], baro_var_[c], res, noise_var, resp);
  }
  for (std::size_t b = 0; b < (sparse_ ? map_->block_count() : 0); ++b) {
    if (!(bg_mass_[b] > 0.0)) continue;
    bg_baro_var_[b] += walk2;
    const double res = altitude_ - bg_baro_mu_[b] - elevation(map_->block_level(b));
    double resp = 0.0;
    const double lik = mixture(res, noise_var + bg_baro_var_[b], kBaroOutlierProb, 1.0 / 20.0, resp);
    block_loglik_[b] = std::log(lik);
    adf_update(bg_baro_mu_[b], bg_baro_var_[b], res, noise_var, resp);
  }
  apply_cell_loglik(t);
}

void GridFilter::change_levels(double prob) {
  const auto conns = map_->connector_cells();
  if (conns.empty() || prob <= 0.0) return;
  const auto row = static_cast<std::size_t>(bins_);
  for (std::size_t i = 0; i < conns.size(); ++i) {
    const std::size_t c = conns[i];
    conn_mass_[i] = mass_[c];
    if (!(mass_[c] > 0.0)) continue;  // nothing leaves it: its copy is not read
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
      if (cell_active_[t] == 0) continue;  // into the background: only mass under the active floor
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

}  // namespace ipe
