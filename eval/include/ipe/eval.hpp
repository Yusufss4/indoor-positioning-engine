#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/trace.hpp"
#include "ipe/trace_player.hpp"
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
  /// recordings whose truth is a few waypoints. Every pair of fixes counts, across floors too.
  double jitter_p50_m = -1.0;   ///< Median |p(t+T) - 2 p(t) + p(t-T)|; walking at steady speed gives ~0.
  double jitter_p95_m = -1.0;   ///< 95th percentile of the same second difference.
  double jumps_per_min = -1.0;  ///< Non-overlapping 1 s windows that moved over 3 m or changed floor.
  /// Moves between consecutive samples on one floor that cross a wall, per minute: the dot going
  /// through a wall (into a room without a door, or a corridor behind a wall). -1 without the venue.
  double wall_crossings_per_min = -1.0;
  int level_flips = 0;  ///< Changes of the reported level; compare with level_changes.
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

/// The replay loop behind replay(), for any engine with PositioningEngine's sensor inputs and
/// estimate(), room_tracker(), take_room_events() and step_count(); `engine` must be fresh. Particle
/// snapshots call `append_snapshot(engine, out, max)`, found by argument-dependent lookup.
template <class Engine>
ReplayResult replay_with(Engine& engine, const Venue& venue, const Trace& trace, const ReplayOptions& opts = {});

/// Up to about `max` of the engine's particles as (x, y, weight, level).
void append_snapshot(const PositioningEngine& engine, std::vector<std::array<float, 4>>& out, std::size_t max);

Metrics compute_metrics(const std::vector<EstimateRecord>& records, double warmup_s);
/// Fills the smoothness fields of `m` from an evenly spaced track (spacing `period_s`). Samples before
/// `t_from` are skipped; `records` (truth) are only used for path_ratio, `venue` (its walls) only for
/// wall_crossings_per_min.
void compute_smoothness(Metrics& m, const std::vector<TrackSample>& track, double period_s,
                        const std::vector<EstimateRecord>& records, double t_from, const Venue* venue = nullptr);
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

template <class Engine>
ReplayResult replay_with(Engine& engine, const Venue& venue, const Trace& trace, const ReplayOptions& opts) {
  using clock = std::chrono::steady_clock;
  ReplayResult result;
  result.records.reserve(trace.truth.size());

  TracePlayer player(venue, trace);
  clock::duration busy{};
  std::uint64_t updates = 0;
  Estimate last_fix;
  std::optional<RoomTracker> truth_rooms;
  if (!venue.rooms().empty()) truth_rooms.emplace(venue, truth_room_config());
  std::vector<std::size_t> truth_in, est_in;
  std::size_t truth_seen = 0;
  const double track_t0 = trace.truth.empty() ? 0.0 : trace.truth.front().t;
  std::size_t track_ticks = 0;
  while (!player.finished()) {
    if (opts.track_period_s > 0.0) {
      // The estimate as it stands after every event up to and including the tick.
      for (double tick = track_t0 + static_cast<double>(track_ticks) * opts.track_period_s; tick < player.next_time();
           tick = track_t0 + static_cast<double>(++track_ticks) * opts.track_period_s) {
        const Estimate e = engine.estimate();
        result.track.push_back({tick, e.pos, e.level, e.valid});
      }
    }
    const auto start = clock::now();
    if (player.step(engine) != TracePlayer::Event::Truth) {
      busy += clock::now() - start;
      ++updates;
    } else {
      const auto& truth = player.truth();
      const Estimate est = engine.estimate();
      if (est.valid) last_fix = est;

      EstimateRecord rec;
      rec.t = truth.t;
      rec.truth = truth.pos;
      rec.truth_level = truth.level;
      rec.has_fix = last_fix.valid;
      if (rec.has_fix) {
        rec.estimate = last_fix.pos;
        rec.sigma_m = last_fix.sigma_radius();
        rec.cov_xx = last_fix.cov_xx;
        rec.cov_xy = last_fix.cov_xy;
        rec.cov_yy = last_fix.cov_yy;
        rec.error_m = distance(last_fix.pos, truth.pos);
        rec.est_level = last_fix.level;
      }
      if (truth_rooms) {
        Estimate at_truth;
        at_truth.pos = truth.pos;
        at_truth.level = truth.level;
        at_truth.valid = true;
        truth_rooms->update(truth.t, at_truth, nullptr);
        truth_rooms->current_rooms(truth_in);
        engine.room_tracker()->current_rooms(est_in);
        rec.rooms_match = truth_in == est_in ? 1 : 0;
        for (const auto& e : engine.take_room_events()) result.room_events.push_back(e);
      }
      result.records.push_back(rec);

      if (opts.particle_snapshot_every > 0 &&
          truth_seen % static_cast<std::size_t>(opts.particle_snapshot_every) == 0) {
        ParticleSnapshot snap;
        snap.t = truth.t;
        append_snapshot(engine, snap.particles, opts.particle_snapshot_max);
        result.snapshots.push_back(std::move(snap));
      }
      ++truth_seen;
    }
  }

  result.metrics = compute_metrics(result.records, opts.warmup_s);
  if (!result.records.empty()) {
    compute_smoothness(result.metrics, result.track, opts.track_period_s, result.records,
                       result.records.front().t + opts.warmup_s, &venue);
  }
  if (truth_rooms && !trace.truth.empty()) {
    for (const auto& e : engine.take_room_events()) result.room_events.push_back(e);
    result.truth_room_events = truth_rooms->take_events();
    score_room_events(result.metrics, result.room_events, result.truth_room_events,
                      trace.truth.front().t + opts.warmup_s);
  }
  result.metrics.steps = engine.step_count();
  result.metrics.compute_us_per_update =
      updates ? std::chrono::duration<double, std::micro>(busy).count() / static_cast<double>(updates) : 0.0;
  return result;
}

}  // namespace ipe::eval
