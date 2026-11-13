// The reported estimate: level, mean and covariance of the mass, kept on the map.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

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
