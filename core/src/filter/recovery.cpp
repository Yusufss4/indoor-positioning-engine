// Recovery: uniform mass mixed in at a small rate, the way back from a wrong lock.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

void GridFilter::maybe_teleport(double t) {
  if (!teleport_started_) {
    teleport_started_ = true;
    last_teleport_t_ = t;
    return;
  }
  const double dt = t - last_teleport_t_;
  if (dt < cfg_.teleport_period_s) return;
  last_teleport_t_ = t;
  const double rho = std::min(1.0 - std::exp(-std::max(cfg_.teleport_rate_per_s, 0.0) * dt), 1.0);
  if (rho <= 0.0) return;
  const std::size_t n = map_->size();
  const double u = rho / static_cast<double>(n);
  const double dev_var0 = cfg_.estimate_rssi_offset ? cfg_.rssi_offset_sigma * cfg_.rssi_offset_sigma : 0.0;
  const double baro_var0 = cfg_.baro_offset_sigma_m * cfg_.baro_offset_sigma_m;
  const auto row = static_cast<std::size_t>(bins_);
  const double prior = 1.0 / bins_;
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
    for (std::size_t k = 0; k < row; ++k) hc[k] = static_cast<float>((1.0 - f) * hc[k] + f * prior);
  }
}

}  // namespace ipe
