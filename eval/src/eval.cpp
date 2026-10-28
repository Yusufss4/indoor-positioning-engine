#include "ipe/eval.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PdrConfig, gravity, lowpass_alpha, peak_threshold, valley_threshold,
                                                min_step_interval, weinberg_k, min_step_length, max_step_length)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RssiModelConfig, sigma_db, outlier_prob, outlier_range_db, min_distance,
                                                wall_aware)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ParticleFilterConfig, num_particles, seed, step_length_sigma,
                                                step_scale_sigma, step_scale_walk, heading_sigma_deg,
                                                heading_outlier_prob, wall_constraint, estimate_rssi_offset,
                                                rssi_offset_sigma, rssi_offset_walk, beacon_integrity,
                                                integrity_threshold_db, integrity_alpha, integrity_min_samples,
                                                integrity_max_sigma_m, resample_ess_ratio, roughening_m, recovery,
                                                recovery_alpha_slow, recovery_alpha_fast, recovery_max_fraction)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EkfConfig, step_length_sigma_rel, step_length_sigma_abs,
                                                heading_sigma_deg, init_pos_sigma, init_heading_sigma_deg, rssi_min_dbm,
                                                rssi_gate_chi2, heading_gate_chi2, init_min_beacons, init_window_s,
                                                reinit_after_rejections)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WclConfig, window_s, top_k, weight_exponent)

void to_json(nlohmann::json& j, const EngineConfig& c) {
  j = nlohmann::json{{"filter", std::string(to_string(c.filter))},
                     {"pdr", c.pdr},
                     {"rssi", c.rssi},
                     {"pf", c.pf},
                     {"ekf", c.ekf},
                     {"wcl", c.wcl},
                     {"mag_heading_sigma_deg", c.mag_heading_sigma_deg}};
}

void from_json(const nlohmann::json& j, EngineConfig& c) {
  c.filter = parse_filter_kind(j.at("filter").get<std::string>());
  j.at("pdr").get_to(c.pdr);
  j.at("rssi").get_to(c.rssi);
  j.at("pf").get_to(c.pf);
  j.at("ekf").get_to(c.ekf);
  j.at("wcl").get_to(c.wcl);
  j.at("mag_heading_sigma_deg").get_to(c.mag_heading_sigma_deg);
}

namespace eval {
namespace {

void check_known_keys(const nlohmann::json& base, const nlohmann::json& patch, const std::string& path) {
  if (!patch.is_object()) return;
  for (const auto& [key, value] : patch.items()) {
    const std::string full = path.empty() ? key : path + "." + key;
    if (!base.is_object() || !base.contains(key)) throw std::invalid_argument("unknown config key '" + full + "'");
    check_known_keys(base.at(key), value, full);
  }
}

}  // namespace

EngineConfig preset_config(std::string_view preset) {
  EngineConfig cfg;
  if (preset == "pf") {
    cfg.filter = FilterKind::ParticleFilter;
  } else if (preset == "pf-nomap") {
    cfg.filter = FilterKind::ParticleFilter;
    cfg.pf.wall_constraint = false;
    cfg.rssi.wall_aware = false;
    // Without wall loss in the model every beacon behind a wall looks biased.
    cfg.pf.beacon_integrity = false;
  } else if (preset == "ekf") {
    cfg.filter = FilterKind::Ekf;
  } else if (preset == "wcl") {
    cfg.filter = FilterKind::WeightedCentroid;
  } else {
    throw std::invalid_argument("unknown preset '" + std::string(preset) + "' (expected pf, pf-nomap, ekf or wcl)");
  }
  return cfg;
}

const std::vector<std::string>& preset_names() {
  static const std::vector<std::string> names = {"pf", "pf-nomap", "ekf", "wcl"};
  return names;
}

void apply_config_overrides(EngineConfig& cfg, std::string_view json_patch) {
  nlohmann::json j = cfg;
  const auto patch = nlohmann::json::parse(json_patch);
  check_known_keys(j, patch, "");
  j.merge_patch(patch);
  cfg = j.get<EngineConfig>();
}

std::string config_to_json(const EngineConfig& cfg, int indent) { return nlohmann::json(cfg).dump(indent); }

double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const double rank = std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1);
  const auto lo = static_cast<std::size_t>(std::floor(rank));
  const auto hi = std::min(lo + 1, values.size() - 1);
  const double frac = rank - static_cast<double>(lo);
  return values[lo] * (1.0 - frac) + values[hi] * frac;
}

Metrics compute_metrics(const std::vector<EstimateRecord>& records, double warmup_s) {
  Metrics m;
  if (records.empty()) return m;
  const double t0 = records.front().t;

  m.time_to_first_fix_s = -1.0;
  for (const auto& r : records) {
    if (r.has_fix) {
      m.time_to_first_fix_s = r.t - t0;
      break;
    }
  }

  std::vector<double> errors;
  std::size_t evaluated = 0;
  for (const auto& r : records) {
    if (r.t - t0 < warmup_s) continue;
    ++evaluated;
    if (r.has_fix) errors.push_back(r.error_m);
  }
  m.samples = evaluated;
  m.availability = evaluated ? static_cast<double>(errors.size()) / static_cast<double>(evaluated) : 0.0;
  if (!errors.empty()) {
    double sum = 0.0, sum_sq = 0.0;
    for (double e : errors) {
      sum += e;
      sum_sq += e * e;
      m.max_m = std::max(m.max_m, e);
    }
    const auto n = static_cast<double>(errors.size());
    m.mean_m = sum / n;
    m.rmse_m = std::sqrt(sum_sq / n);
    m.cep50_m = percentile(errors, 0.50);
    m.cep95_m = percentile(errors, 0.95);
  }

  // Convergence: earliest time from which the error stays below 3 m for 10 s.
  constexpr double threshold = 3.0;
  constexpr double hold = 10.0;
  m.convergence_s = -1.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (!records[i].has_fix || records[i].error_m >= threshold) continue;
    bool ok = true;
    std::size_t j = i;
    for (; j < records.size() && records[j].t - records[i].t <= hold; ++j) {
      if (!records[j].has_fix || records[j].error_m >= threshold) {
        ok = false;
        break;
      }
    }
    if (ok && j < records.size()) {
      m.convergence_s = records[i].t - t0;
      break;
    }
  }
  return m;
}

ReplayResult replay(const Venue& venue, const Trace& trace, const EngineConfig& cfg, const ReplayOptions& opts) {
  using clock = std::chrono::steady_clock;
  PositioningEngine engine(venue, cfg);
  ReplayResult result;
  result.filter = std::string(to_string(cfg.filter));
  result.records.reserve(trace.truth.size());

  // Beacon ids are resolved once so the hot loop works with indices only.
  std::vector<std::size_t> ble_index(trace.ble.size());
  for (std::size_t k = 0; k < trace.ble.size(); ++k) {
    ble_index[k] = venue.beacon_index(trace.ble[k].beacon_id).value_or(venue.beacons().size());
  }

  clock::duration busy{};
  std::uint64_t updates = 0;
  Estimate last_fix;
  std::size_t i = 0, b = 0, g = 0, truth_seen = 0;
  while (g < trace.truth.size()) {
    const double ti = i < trace.imu.size() ? trace.imu[i].t : 1e300;
    const double tb = b < trace.ble.size() ? trace.ble[b].t : 1e300;
    const double tg = trace.truth[g].t;

    if (ti <= tb && ti <= tg) {
      const auto start = clock::now();
      engine.on_imu(trace.imu[i++]);
      busy += clock::now() - start;
      ++updates;
    } else if (tb <= tg) {
      const auto start = clock::now();
      engine.on_ble(trace.ble[b].t, ble_index[b], trace.ble[b].rssi);
      busy += clock::now() - start;
      ++b;
      ++updates;
    } else {
      const auto& truth = trace.truth[g++];
      const Estimate est = engine.estimate();
      if (est.valid) last_fix = est;

      EstimateRecord rec;
      rec.t = truth.t;
      rec.truth = truth.pos;
      rec.has_fix = last_fix.valid;
      if (rec.has_fix) {
        rec.estimate = last_fix.pos;
        rec.sigma_m = last_fix.sigma_radius();
        rec.error_m = distance(last_fix.pos, truth.pos);
      }
      result.records.push_back(rec);

      const auto* pf = engine.particle_filter();
      if (pf && opts.particle_snapshot_every > 0 &&
          truth_seen % static_cast<std::size_t>(opts.particle_snapshot_every) == 0) {
        ParticleSnapshot snap;
        snap.t = truth.t;
        const auto& ps = pf->particles();
        const std::size_t stride =
            std::max<std::size_t>(1, ps.size() / std::max<std::size_t>(1, opts.particle_snapshot_max));
        for (std::size_t k = 0; k < ps.size(); k += stride) {
          snap.particles.push_back(
              {static_cast<float>(ps[k].pos.x), static_cast<float>(ps[k].pos.y), static_cast<float>(ps[k].weight)});
        }
        result.snapshots.push_back(std::move(snap));
      }
      ++truth_seen;
    }
  }

  result.metrics = compute_metrics(result.records, opts.warmup_s);
  result.metrics.steps = engine.step_count();
  result.metrics.compute_us_per_update =
      updates ? std::chrono::duration<double, std::micro>(busy).count() / static_cast<double>(updates) : 0.0;
  return result;
}

std::string metrics_to_json(const Metrics& m, int indent) {
  const nlohmann::json j = {{"samples", m.samples},
                            {"availability", m.availability},
                            {"time_to_first_fix_s", m.time_to_first_fix_s},
                            {"convergence_s", m.convergence_s},
                            {"mean_m", m.mean_m},
                            {"rmse_m", m.rmse_m},
                            {"cep50_m", m.cep50_m},
                            {"cep95_m", m.cep95_m},
                            {"max_m", m.max_m},
                            {"steps", m.steps},
                            {"compute_us_per_update", m.compute_us_per_update}};
  return j.dump(indent);
}

void write_estimates_csv(const std::string& path, const ReplayResult& result) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fprintf(f, "t,truth_x,truth_y,est_x,est_y,sigma_m,error_m,has_fix\n");
  for (const auto& r : result.records) {
    std::fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", r.t, r.truth.x, r.truth.y, r.estimate.x, r.estimate.y,
                 r.sigma_m, r.error_m, r.has_fix ? 1 : 0);
  }
  std::fclose(f);
}

void write_particles_csv(const std::string& path, const ReplayResult& result) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fprintf(f, "t,x,y,w\n");
  for (const auto& s : result.snapshots) {
    for (const auto& p : s.particles) std::fprintf(f, "%.3f,%.3f,%.3f,%.6g\n", s.t, p[0], p[1], p[2]);
  }
  std::fclose(f);
}

}  // namespace eval
}  // namespace ipe
