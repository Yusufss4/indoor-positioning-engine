#include "ipe/level_selector.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {

int LevelSelector::update(double t, std::span<const double> weights) {
  if (weights.empty()) return 0;
  const auto top = static_cast<int>(std::max_element(weights.begin(), weights.end()) - weights.begin());
  if (level_ < 0 || avg_.size() != weights.size()) {
    avg_.assign(weights.begin(), weights.end());
    t_ = t;
    level_ = top;
    return level_;
  }
  const double dt = std::max(t - t_, 0.0);
  const double alpha = smoothing_s_ > 0.0 ? 1.0 - std::exp(-dt / smoothing_s_) : 1.0;
  t_ = t;
  for (std::size_t l = 0; l < avg_.size(); ++l) avg_[l] += alpha * (weights[l] - avg_[l]);
  const auto best = static_cast<std::size_t>(std::max_element(avg_.begin(), avg_.end()) - avg_.begin());
  const auto held = static_cast<std::size_t>(level_);
  if (weights[held] <= 0.0) {
    // No mass left on the reported level: nothing to report there, whatever the average says.
    level_ = top;
  } else if (best != held && avg_[best] - avg_[held] >= hysteresis_) {
    level_ = static_cast<int>(best);
  }
  return level_;
}

}  // namespace ipe
