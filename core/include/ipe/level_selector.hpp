#pragma once

#include <span>
#include <vector>

namespace ipe {

/// Chooses the level to report from per-level weights that can swing with every measurement. The
/// weights are averaged with time constant `smoothing_s`, and the reported level only moves once another
/// level leads it by `hysteresis` in that average, or the reported level has no weight left at all.
/// With both 0 it reports the most likely level of every update. No allocation after the first update.
class LevelSelector {
 public:
  LevelSelector(double smoothing_s, double hysteresis) : smoothing_s_(smoothing_s), hysteresis_(hysteresis) {}

  /// Forgets the history; the next update reports the most likely level.
  void reset() { level_ = -1; }
  /// Feeds the weights at time `t` (one per level, summing to 1) and returns the level to report.
  int update(double t, std::span<const double> weights);
  [[nodiscard]] int level() const { return level_; }

 private:
  double smoothing_s_;
  double hysteresis_;
  int level_ = -1;
  double t_ = 0.0;
  std::vector<double> avg_;
};

}  // namespace ipe
