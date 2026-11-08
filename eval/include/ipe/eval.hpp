#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/trace.hpp"
#include "ipe/venue.hpp"

namespace ipe::eval {

/// Named engine configurations. The only one is `pf`: the particle filter with
/// wall constraint, wall-aware RSSI and the device offset state. Variants are
/// config patches (`--config`, `--set`) on top of it.
EngineConfig preset_config(std::string_view preset);
const std::vector<std::string>& preset_names();

/// RFC 7386 merge patch on top of `cfg`. Unknown keys throw, so typos in
/// experiment configs fail loudly instead of silently using defaults.
void apply_config_overrides(EngineConfig& cfg, std::string_view json_patch);
std::string config_to_json(const EngineConfig& cfg, int indent = 2);

struct Metrics {
  std::size_t samples = 0;    ///< Ground-truth samples evaluated (after warm-up).
  double availability = 0.0;  ///< Fraction of evaluated samples with a position fix.
  double time_to_first_fix_s = 0.0;
  double convergence_s = 0.0;  ///< First time error stays < 3 m for 10 s (-1 if never).
  double mean_m = 0.0;
  double rmse_m = 0.0;
  double cep50_m = 0.0;
  double cep95_m = 0.0;
  double max_m = 0.0;
  std::uint64_t steps = 0;
  double compute_us_per_update = 0.0;  ///< Wall-clock, not deterministic.

  /// Fraction of evaluated samples with a fix on the true level. Horizontal
  /// error statistics above only include those samples; time on the wrong
  /// floor shows up here instead of as a huge metric error.
  double floor_accuracy = 1.0;
  /// Mean delay from a true level change until the estimate reports the new
  /// level (-1 if the walk never changes level). An estimate that never
  /// catches up counts the time until the next true change.
  double floor_switch_latency_s = -1.0;
  int level_changes = 0;

  /// Uncertainty calibration over the scored fixes, from NEES = e^T P^-1 e
  /// (horizontal error against the reported covariance). An honest filter has
  /// coverage95 ~ 0.95 and nees_median ~ 1.39 (chi-square, 2 dof). Both are -1
  /// when no scored fix has a covariance.
  double coverage95 = -1.0;  ///< Share of fixes with NEES <= 5.991.
  double nees_median = -1.0;

  /// Room Enter/Exit events against ground truth (see score_room_events).
  /// The ratios are -1 for venues without rooms; with nothing to score they are 1.
  int room_events = 0;                        ///< True events after warm-up.
  double room_event_precision = -1.0;         ///< Share of estimated events matching a true one.
  double room_event_recall = -1.0;            ///< Share of true events detected.
  double room_event_latency_median_s = -1.0;  ///< Estimated minus true event time, matched events.
  double room_accuracy = -1.0;                ///< Share of evaluated samples with the true set of rooms.

  /// Smoothness of the reported track, from the estimate sampled every ReplayOptions::track_period_s
  /// (see compute_smoothness). They need no ground truth except path_ratio, so they also work on
  /// recordings whose truth is a few waypoints. Floor changes are excluded from the horizontal figures.
  double jitter_p50_m = -1.0;   ///< Median |p(t+T) - 2 p(t) + p(t-T)|; walking at steady speed gives ~0.
  double jitter_p95_m = -1.0;   ///< 95th percentile of the same second difference.
  double jumps_per_min = -1.0;  ///< Non-overlapping 1 s windows in which the estimate moved more than 3 m.
  int level_flips = 0;          ///< Changes of the reported level; compare with level_changes.
  /// Length of the estimated track over the length of the true one, after warm-up; -1 unless the
  /// truth is dense (median spacing <= 0.5 s), since waypoint polylines cut corners.
  double path_ratio = -1.0;
};

struct EstimateRecord {
  double t = 0.0;
  Vec2 truth;
  Vec2 estimate;
  double sigma_m = 0.0;
  double cov_xx = 0.0;
  double cov_xy = 0.0;
  double cov_yy = 0.0;
  double error_m = 0.0;
  bool has_fix = false;
  int truth_level = 0;
  int est_level = 0;
  /// 1 if the engine's current rooms equal the true ones, 0 if not, -1 for venues without rooms.
  int rooms_match = -1;
};

/// The engine's estimate at one tick of the replay's track clock.
struct TrackSample {
  double t = 0.0;
  Vec2 pos;
  int level = 0;
  bool valid = false;
};

struct ParticleSnapshot {
  double t = 0.0;
  std::vector<std::array<float, 4>> particles;  ///< x, y, weight, level
};

struct ReplayOptions {
  double warmup_s = 5.0;            ///< Samples before this are excluded from error statistics.
  int particle_snapshot_every = 0;  ///< Every N truth samples; 0 disables.
  std::size_t particle_snapshot_max = 300;
  double track_period_s = 0.2;  ///< Spacing of ReplayResult::track; 0 disables it and the smoothness metrics.
};

struct ReplayResult {
  std::vector<EstimateRecord> records;
  std::vector<ParticleSnapshot> snapshots;
  /// The estimate every `track_period_s` of trace time, from the first truth sample on: what a client
  /// polling the engine would draw.
  std::vector<TrackSample> track;
  std::vector<RoomEvent> room_events;        ///< From the engine.
  std::vector<RoomEvent> truth_room_events;  ///< From the ground-truth positions.
  Metrics metrics;
};

/// Feeds the trace through a fresh engine in timestamp order and scores the
/// estimate against ground truth at every truth sample. Between fixes the
/// last valid estimate is held, as a blue dot would be on screen.
ReplayResult replay(const Venue& venue, const Trace& trace, const EngineConfig& cfg, const ReplayOptions& opts = {});

Metrics compute_metrics(const std::vector<EstimateRecord>& records, double warmup_s);
/// Fills the smoothness fields of `m` from an evenly spaced track (spacing `period_s`). Samples before
/// `t_from` are skipped; `records` (truth) are only used for path_ratio.
void compute_smoothness(Metrics& m, const std::vector<TrackSample>& track, double period_s,
                        const std::vector<EstimateRecord>& records, double t_from);
double percentile(std::vector<double> values, double p);

/// Fills the room_event_* fields. A true event is detected by the earliest
/// unmatched estimated event of the same room and type within `window_s` of
/// it. Only events at or after `t_from` are counted, but they may match
/// events before it.
void score_room_events(Metrics& m, const std::vector<RoomEvent>& estimated, const std::vector<RoomEvent>& truth,
                       double t_from, double window_s = 5.0);

std::string metrics_to_json(const Metrics& m, int indent = 2);
void write_estimates_csv(const std::string& path, const ReplayResult& result);
void write_particles_csv(const std::string& path, const ReplayResult& result);
/// t,x,y,level,valid: the replay's evenly spaced estimate track.
void write_track_csv(const std::string& path, const ReplayResult& result);
/// t,source,room_id,room_name,type,probability with source "estimate" or "truth", sorted by time.
void write_room_events_csv(const std::string& path, const Venue& venue, const ReplayResult& result);

}  // namespace ipe::eval
