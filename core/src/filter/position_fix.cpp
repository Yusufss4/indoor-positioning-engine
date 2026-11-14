// A position known from outside the engine: a tap on the map, a tag at a known place, a GNSS fix at a door.

#include <algorithm>
#include <cmath>
#include <limits>

#include "detail.hpp"
#include "ipe/grid_filter.hpp"

namespace ipe {

using namespace filter_detail;

void GridFilter::on_position_fix(double t, Vec2 p, int level, double sigma_m) {
  const int levels = map_->venue().level_count();
  if (level >= levels || !std::isfinite(p.x) || !std::isfinite(p.y)) return;
  now_ = t;
  // The fix must be able to move an estimate that is confidently somewhere else, where no mass is left to
  // reweight: a share of the prior goes in first. Mass that is already near the fix outweighs it and keeps
  // what it has learned (heading, device offset, barometer offset).
  mix_uniform(kFixPriorShare);
  const double sigma = std::max(sigma_m, map_->cell_m());
  const double inv = -0.5 / (sigma * sigma);
  for (std::size_t c = 0; c < map_->size(); ++c) {
    if (level >= 0 && map_->cell(c).level != level) {
      loglik_[c] = -std::numeric_limits<double>::infinity();
      continue;
    }
    const double dx = cx_[c] - p.x, dy = cy_[c] - p.y;
    loglik_[c] = inv * (dx * dx + dy * dy);
  }
  localized_ = true;
  apply_cell_loglik(t);
  update_heading(cloud_.level);
}

}  // namespace ipe
