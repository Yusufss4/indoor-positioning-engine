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

/// Named engine configurations compared throughout the project:
///   pf        particle filter with wall constraint, wall-aware RSSI and offset state
///   pf-nomap  same particle filter without any floor-plan knowledge
///   ekf       map-free extended Kalman filter
///   wcl       RSSI-only weighted centroid
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
  /// for filters that report no covariance.
  double coverage95 = -1.0;  ///< Share of fixes with NEES <= 5.991.
  double nees_median = -1.0;

  /// Room Enter/Exit events against ground truth (see score_room_events).
  /// The ratios are -1 for venues without rooms; with nothing to score they are 1.
  int room_events = 0;                        ///< True events after warm-up.
  double room_event_precision = -1.0;         ///< Share of estimated events matching a true one.
  double room_event_recall = -1.0;            ///< Share of true events detected.
  double room_event_latency_median_s = -1.0;  ///< Estimated minus true event time, matched events.
  double room_accuracy = -1.0;                ///< Share of evaluated samples with the true set of rooms.
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

struct ParticleSnapshot {
  double t = 0.0;
  std::vector<std::array<float, 4>> particles;  ///< x, y, weight, level
};

struct ReplayOptions {
  double warmup_s = 5.0;            ///< Samples before this are excluded from error statistics.
  int particle_snapshot_every = 0;  ///< Every N truth samples; 0 disables.
  std::size_t particle_snapshot_max = 300;
};

struct ReplayResult {
  std::string filter;
  std::vector<EstimateRecord> records;
  std::vector<ParticleSnapshot> snapshots;
  std::vector<RoomEvent> room_events;        ///< From the engine.
  std::vector<RoomEvent> truth_room_events;  ///< From the ground-truth positions.
  Metrics metrics;
};

/// Feeds the trace through a fresh engine in timestamp order and scores the
/// estimate against ground truth at every truth sample. Between fixes the
/// last valid estimate is held, as a blue dot would be on screen.
ReplayResult replay(const Venue& venue, const Trace& trace, const EngineConfig& cfg, const ReplayOptions& opts = {});

Metrics compute_metrics(const std::vector<EstimateRecord>& records, double warmup_s);
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
/// t,source,room_id,room_name,type,probability with source "estimate" or "truth", sorted by time.
void write_room_events_csv(const std::string& path, const Venue& venue, const ReplayResult& result);

}  // namespace ipe::eval
