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
};

struct EstimateRecord {
  double t = 0.0;
  Vec2 truth;
  Vec2 estimate;
  double sigma_m = 0.0;
  double error_m = 0.0;
  bool has_fix = false;
};

struct ParticleSnapshot {
  double t = 0.0;
  std::vector<std::array<float, 3>> particles;  ///< x, y, weight
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
  Metrics metrics;
};

/// Feeds the trace through a fresh engine in timestamp order and scores the
/// estimate against ground truth at every truth sample. Between fixes the
/// last valid estimate is held, as a blue dot would be on screen.
ReplayResult replay(const Venue& venue, const Trace& trace, const EngineConfig& cfg, const ReplayOptions& opts = {});

Metrics compute_metrics(const std::vector<EstimateRecord>& records, double warmup_s);
double percentile(std::vector<double> values, double p);

std::string metrics_to_json(const Metrics& m, int indent = 2);
void write_estimates_csv(const std::string& path, const ReplayResult& result);
void write_particles_csv(const std::string& path, const ReplayResult& result);

}  // namespace ipe::eval
