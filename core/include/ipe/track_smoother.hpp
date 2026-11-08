#pragma once

#include "ipe/filter.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct TrackSmootherConfig {
  bool enabled = true;
  /// Radio corrections are absorbed with this time constant instead of drawn at once [s].
  double tau_s = 0.5;
  /// Cap on how fast a correction moves the dot [m/s]; 0 is no cap.
  double max_correction_mps = 0.0;
  /// Further than this from the filter's estimate the dot jumps there instead of gliding [m].
  double snap_m = 10.0;
  /// Between steps the dot keeps moving at the pace of the last step for at most this many step
  /// intervals, instead of waiting for the next step event. 0 draws one jump per step. Off by default:
  /// it overshoots where the walker stops or turns (experiment 8).
  double extrapolate_steps = 0.0;
  /// Steps further apart than this mean the walker had stopped: no pace is taken from them [s].
  double max_step_interval_s = 1.2;
};

/// The position a client draws, made from the particle filter's estimate without lagging behind it:
///   - a step moves the dot by the filter's own displacement for that step, immediately;
///   - between steps it continues at the last step's pace (`extrapolate_steps`);
///   - every other update pulls it towards the filter's estimate with time constant `tau_s`, at most
///     `max_correction_mps`, so radio noise is not drawn as motion;
///   - it jumps to the filter's estimate when that is more than `snap_m` away, when the reported level
///     changes, and whenever gliding would cross a wall or leave the walkable area, so it never slides
///     through a wall.
/// Level, heading and validity are the filter's; the covariance is the filter's taken about the drawn
/// point. Deterministic, O(1) per event plus at most two wall checks, no allocation.
class TrackSmoother {
 public:
  TrackSmoother(const Venue& venue, TrackSmootherConfig cfg) : venue_(&venue), cfg_(cfg) {}

  /// A step moved the filter's estimate from `before` to `after`.
  void on_step(double t, const Estimate& before, const Estimate& after);
  /// Any other filter update; `pf` is the estimate after it.
  void on_update(double t, const Estimate& pf);
  /// The drawn estimate at time `now` (the latest event time), given the filter's current one.
  [[nodiscard]] Estimate estimate(double now, const Estimate& pf) const;

  [[nodiscard]] const TrackSmootherConfig& config() const { return cfg_; }

 private:
  void snap(double t, const Estimate& pf);
  /// Moves the anchor towards `pf` over the time since the last correction.
  void correct(double t, const Estimate& pf);
  /// Moves the anchor to `to` unless that leaves the map or crosses a wall; then snaps to `pf`.
  void move_to(double t, Vec2 to, const Estimate& pf);
  [[nodiscard]] bool walkable_move(Vec2 from, Vec2 to) const;

  const Venue* venue_;
  TrackSmootherConfig cfg_;
  bool valid_ = false;
  Vec2 anchor_;  ///< The drawn position as of the last step or update, before extrapolation.
  int level_ = 0;
  double t_correct_ = 0.0;  ///< Time of the last correction.
  double t_step_ = 0.0;     ///< Time of the last step, NaN-free: only read when `pace_valid_`.
  double step_dt_ = 0.0;    ///< Interval that ended at the last step.
  Vec2 pace_;               ///< Velocity of the last step [m/s].
  bool pace_valid_ = false;
};

}  // namespace ipe
