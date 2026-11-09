#include "ipe/track_smoother.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {

bool TrackSmoother::walkable_move(Vec2 from, Vec2 to) const {
  return venue_->inside(to, level_) && !venue_->crosses_wall(from, to, level_);
}

void TrackSmoother::snap(double t, const Estimate& pf) {
  valid_ = pf.valid;
  from_ = target_ = pf.pos;
  level_ = pf.level;
  t_from_ = t;
  reach_ = 0.0;
}

Vec2 TrackSmoother::glide(double t) const {
  const Vec2 gap = target_ - from_;
  const double d = std::sqrt(gap.norm2());
  const double dt = std::max(t - t_from_, 0.0);
  if (d == 0.0 || dt == 0.0 || reach_ <= 0.0) return from_;
  // Speed min(gap / tau, cap): at the cap while the gap is longer than cap * tau, exponential after that.
  const double tau = cfg_.tau_s;
  const double cap = cfg_.max_correction_mps;
  double covered = d;
  if (cap <= 0.0) {
    if (tau > 0.0) covered = d * (1.0 - std::exp(-dt / tau));
  } else if (tau <= 0.0) {
    covered = std::min(d, cap * dt);
  } else if (d <= cap * tau) {
    covered = d * (1.0 - std::exp(-dt / tau));
  } else {
    const double linear_s = (d - cap * tau) / cap;
    covered = dt <= linear_s ? cap * dt : d - cap * tau * std::exp(-(dt - linear_s) / tau);
  }
  return from_ + gap * (std::min(covered, reach_) / d);
}

Vec2 TrackSmoother::extrapolation(double t) const {
  if (cfg_.extrapolate_steps <= 0.0 || !pace_valid_) return {};
  const double since = std::max(t - t_step_, 0.0);
  // Centred on the filter's estimate: from half the extrapolated distance behind it at a step to half
  // ahead at the next. Radio keeps the estimate on the walker's mean position over a step interval, so
  // running ahead of it by the time since the step would draw the dot half a step too far.
  const double span = cfg_.extrapolate_steps * step_dt_;
  Vec2 off = pace_ * (std::min(since, span) - 0.5 * span);
  // No step for a while: the walker has stopped, so the dot goes back to where the filter has it.
  const double late = since - cfg_.max_step_interval_s;
  if (late > 0.0) off = off * (cfg_.tau_s > 0.0 ? std::exp(-late / cfg_.tau_s) : 0.0);
  return off;
}

void TrackSmoother::retarget(double t, Vec2 from, const Estimate& pf) {
  const double d = distance(from, pf.pos);
  if (d > cfg_.snap_m) {
    snap(t, pf);
    return;
  }
  from_ = from;
  target_ = pf.pos;
  t_from_ = t;
  reach_ = d;
  if (walkable_move(from, pf.pos)) return;
  // The estimate is behind a wall. Glide up to the wall, not through it: the estimate often comes back
  // before the dot gets there (a cloud split by a wall). Once the dot is at the wall, jump.
  const Vec2 dir = (pf.pos - from) * (1.0 / d);
  double lo = 0.0, hi = d;
  for (int i = 0; i < kWallSearchSteps; ++i) {
    const double mid = 0.5 * (lo + hi);
    (walkable_move(from, from + dir * mid) ? lo : hi) = mid;
  }
  reach_ = lo - kWallMarginM;
  if (reach_ <= 0.0) snap(t, pf);
}

void TrackSmoother::on_step(double t, const Estimate& before, const Estimate& after, Vec2 motion) {
  const bool continuous = valid_ && after.valid && before.valid && before.level == after.level && after.level == level_;
  if (!continuous) {
    snap(t, after);
    pace_valid_ = false;
    t_step_ = t;
    return;
  }
  // The step's motion replaces what extrapolation drew since the previous step.
  const Vec2 here = glide(t);
  const double interval = t - t_step_;
  pace_valid_ = interval > 0.0 && interval <= cfg_.max_step_interval_s;
  if (pace_valid_) {
    step_dt_ = interval;
    pace_ = motion * (1.0 / interval);
  }
  t_step_ = t;
  const Vec2 moved = here + motion;
  if (!walkable_move(here, moved)) {
    snap(t, after);
    return;
  }
  retarget(t, moved, after);
}

void TrackSmoother::on_update(double t, const Estimate& pf) {
  if (!pf.valid) {
    valid_ = false;
    return;
  }
  if (!valid_ || pf.level != level_) {
    snap(t, pf);
    pace_valid_ = false;
    return;
  }
  retarget(t, glide(t), pf);
}

Estimate TrackSmoother::estimate(double now, const Estimate& pf) const {
  if (!valid_ || !pf.valid || pf.level != level_) return pf;
  Vec2 pos = glide(now);
  const Vec2 ext = extrapolation(now);
  if (ext.x != 0.0 || ext.y != 0.0) {
    const Vec2 ahead = pos + ext;
    if (walkable_move(pos, ahead)) pos = ahead;
  }
  Estimate e = pf;
  e.t = std::max(pf.t, now);
  const Vec2 off = pos - pf.pos;
  e.pos = pos;
  e.cov_xx += off.x * off.x;
  e.cov_xy += off.x * off.y;
  e.cov_yy += off.y * off.y;
  return e;
}

}  // namespace ipe
