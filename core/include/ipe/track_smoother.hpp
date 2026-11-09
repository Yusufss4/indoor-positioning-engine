#pragma once

#include "ipe/filter.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct TrackSmootherConfig {
  bool enabled = true;
  /// Radio corrections are absorbed with this time constant instead of drawn at once [s].
  double tau_s = 0.5;
  /// Cap on how fast a correction moves the dot, on top of the walking [m/s]; 0 is no cap. Off by default:
  /// it holds the dot back while BLE moves the estimate to new evidence. The Wi-Fi configs set 1.5, where
  /// one scan can move the estimate several metres (experiment 11).
  double max_correction_mps = 0.0;
  /// Further than this from the filter's estimate the dot jumps there instead of gliding [m].
  double snap_m = 10.0;
  /// Between steps the dot keeps moving at the pace of the last step for at most this many step
  /// intervals, centred on the filter's estimate, instead of waiting for the next step event. 0 draws one
  /// jump per step.
  double extrapolate_steps = 1.0;
  /// Steps further apart than this mean the walker had stopped: no pace is taken from them, and an
  /// extrapolated offset older than this is taken back with time constant `tau_s` [s].
  double max_step_interval_s = 1.2;
};

/// The position a client draws, made from the particle filter's estimate without lagging behind it:
///   - a step moves the dot by the step's own motion (ParticleFilter::step_motion()), immediately;
///   - between steps it continues at the last step's pace (`extrapolate_steps`);
///   - everything else that moves the filter's estimate, including what a step does besides moving
///     (particles dying at walls, resampling), is a correction: the dot glides towards the estimate in
///     continuous time, `exp(-dt / tau_s)` of the gap left after `dt`, at most `max_correction_mps`, so
///     a correction is never drawn at the instant it arrives, whatever the update rate;
///   - it jumps to the filter's estimate when that is more than `snap_m` away and when the reported level
///     changes. An estimate behind a wall (or off the walkable area) is glided towards only up to the
///     wall; if it is still there once the dot has reached the wall, the dot jumps. It never slides
///     through a wall.
/// Level, heading and validity are the filter's; the covariance is the filter's taken about the drawn
/// point. Deterministic, O(1) per event plus a few wall checks (a short bisection when an estimate is
/// behind a wall), no allocation.
class TrackSmoother {
 public:
  TrackSmoother(const Venue& venue, TrackSmootherConfig cfg) : venue_(&venue), cfg_(cfg) {}

  /// A step moved the filter's estimate from `before` to `after`; `motion` is the step's own displacement.
  void on_step(double t, const Estimate& before, const Estimate& after, Vec2 motion);
  /// Any other filter update; `pf` is the estimate after it.
  void on_update(double t, const Estimate& pf);
  /// The drawn estimate at time `now` (the latest event time), given the filter's current one.
  [[nodiscard]] Estimate estimate(double now, const Estimate& pf) const;

  [[nodiscard]] const TrackSmootherConfig& config() const { return cfg_; }

 private:
  void snap(double t, const Estimate& pf);
  /// The glide from `from_` towards `target_`, evaluated at `t` (before extrapolation).
  [[nodiscard]] Vec2 glide(double t) const;
  /// Offset of the extrapolated dot from the glide at `t`.
  [[nodiscard]] Vec2 extrapolation(double t) const;
  /// Starts a glide from `from` at `t` towards `pf` (up to the first wall in the way), or snaps to `pf` when
  /// that is more than `snap_m` away or the dot already stands at that wall.
  void retarget(double t, Vec2 from, const Estimate& pf);
  [[nodiscard]] bool walkable_move(Vec2 from, Vec2 to) const;

  static constexpr int kWallSearchSteps = 12;  ///< Bisection steps for where a glide meets a wall.
  static constexpr double kWallMarginM = 0.1;  ///< The glide stops this far before the wall.

  const Venue* venue_;
  TrackSmootherConfig cfg_;
  bool valid_ = false;
  int level_ = 0;
  Vec2 from_;             ///< Where the glide started (the drawn point at `t_from_`, before extrapolation).
  Vec2 target_;           ///< The filter's estimate the dot glides towards.
  double t_from_ = 0.0;   ///< Start of the current glide.
  double reach_ = 0.0;    ///< How far along from_ -> target_ the glide may go: short of a wall in the way [m].
  double t_step_ = 0.0;   ///< Time of the last step, only read when `pace_valid_`.
  double step_dt_ = 0.0;  ///< Interval that ended at the last step.
  Vec2 pace_;             ///< Velocity of the last step's motion [m/s].
  bool pace_valid_ = false;
};

}  // namespace ipe
