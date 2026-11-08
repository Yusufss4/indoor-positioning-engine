#include "ipe/track_smoother.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {

bool TrackSmoother::walkable_move(Vec2 from, Vec2 to) const {
  return venue_->inside(to, level_) && !venue_->crosses_wall(from, to, level_);
}

void TrackSmoother::snap(double t, const Estimate& pf) {
  valid_ = pf.valid;
  anchor_ = pf.pos;
  level_ = pf.level;
  t_correct_ = t;
}

void TrackSmoother::move_to(double t, Vec2 to, const Estimate& pf) {
  if (walkable_move(anchor_, to)) {
    anchor_ = to;
  } else {
    snap(t, pf);
  }
}

void TrackSmoother::correct(double t, const Estimate& pf) {
  const double dt = std::max(t - t_correct_, 0.0);
  t_correct_ = t;
  const Vec2 err = pf.pos - anchor_;
  const double d = std::sqrt(err.norm2());
  if (d > cfg_.snap_m) {
    snap(t, pf);
    return;
  }
  if (d == 0.0 || dt == 0.0) return;
  double gain = cfg_.tau_s > 0.0 ? 1.0 - std::exp(-dt / cfg_.tau_s) : 1.0;
  if (cfg_.max_correction_mps > 0.0) gain = std::min(gain, cfg_.max_correction_mps * dt / d);
  move_to(t, anchor_ + err * gain, pf);
}

void TrackSmoother::on_step(double t, const Estimate& before, const Estimate& after) {
  const bool continuous = valid_ && after.valid && before.valid && before.level == after.level && after.level == level_;
  if (!continuous) {
    snap(t, after);
    pace_valid_ = false;
    t_step_ = t;
    return;
  }
  const Vec2 delta = after.pos - before.pos;
  const double interval = t - t_step_;
  pace_valid_ = interval > 0.0 && interval <= cfg_.max_step_interval_s;
  if (pace_valid_) {
    step_dt_ = interval;
    pace_ = delta * (1.0 / interval);
  }
  t_step_ = t;
  move_to(t, anchor_ + delta, after);
  if (valid_ && after.level == level_) correct(t, after);
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
  correct(t, pf);
}

Estimate TrackSmoother::estimate(double now, const Estimate& pf) const {
  if (!valid_ || !pf.valid || pf.level != level_) return pf;
  Vec2 pos = anchor_;
  if (cfg_.extrapolate_steps > 0.0 && pace_valid_) {
    const double ahead = std::clamp(now - t_step_, 0.0, cfg_.extrapolate_steps * step_dt_);
    const Vec2 ext = anchor_ + pace_ * ahead;
    if (ahead > 0.0 && walkable_move(anchor_, ext)) pos = ext;
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
