#include "ipe/eval.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PdrConfig, gravity, lowpass_alpha, peak_threshold, valley_threshold,
                                                min_step_interval, weinberg_k, min_step_length, max_step_length)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RssiModelConfig, sigma_db, outlier_prob, outlier_range_db, min_distance,
                                                wall_aware)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(BaroConfig, lowpass_tau_s, sea_level_hpa)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ParticleFilterConfig, num_particles, seed, step_length_sigma, step_scale_sigma, step_scale_walk, heading_sigma_deg,
    heading_outlier_prob, wall_constraint, estimate_rssi_offset, rssi_offset_sigma, rssi_offset_walk, beacon_integrity,
    integrity_threshold_db, integrity_alpha, integrity_min_samples, integrity_max_sigma_m, resample_ess_ratio,
    roughening_m, recovery, recovery_alpha_slow, recovery_alpha_fast, recovery_max_fraction, init_proposal,
    init_proposal_fraction, init_window_s, recovery_proposal, proposal_history, proposal_max_age_s, proposal_level_jump,
    proposal_ring_scale, scan_tempering, level_change_prob_step, level_change_prob_baro, use_barometer,
    baro_update_interval_s, baro_sigma_m, baro_outlier_prob, baro_offset_sigma_m, baro_offset_walk_m)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EkfConfig, step_length_sigma_rel, step_length_sigma_abs,
                                                heading_sigma_deg, init_pos_sigma, init_heading_sigma_deg, rssi_min_dbm,
                                                rssi_gate_chi2, heading_gate_chi2, init_min_beacons, init_window_s,
                                                reinit_after_rejections)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WclConfig, window_s, top_k, weight_exponent)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RoomEventConfig, enter_prob, exit_prob, enter_dwell_s, exit_dwell_s,
                                                update_period_s)

void to_json(nlohmann::json& j, const EngineConfig& c) {
  j = nlohmann::json{{"filter", std::string(to_string(c.filter))},
                     {"pdr", c.pdr},
                     {"rssi", c.rssi},
                     {"baro", c.baro},
                     {"pf", c.pf},
                     {"ekf", c.ekf},
                     {"wcl", c.wcl},
                     {"rooms", c.rooms},
                     {"mag_heading_sigma_deg", c.mag_heading_sigma_deg}};
}

void from_json(const nlohmann::json& j, EngineConfig& c) {
  c.filter = parse_filter_kind(j.at("filter").get<std::string>());
  j.at("pdr").get_to(c.pdr);
  j.at("rssi").get_to(c.rssi);
  if (j.contains("baro")) j.at("baro").get_to(c.baro);
  j.at("pf").get_to(c.pf);
  j.at("ekf").get_to(c.ekf);
  j.at("wcl").get_to(c.wcl);
  if (j.contains("rooms")) j.at("rooms").get_to(c.rooms);
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

  std::vector<double> errors, nees;
  std::size_t evaluated = 0, with_fix = 0, room_scored = 0, room_ok = 0;
  for (const auto& r : records) {
    if (r.t - t0 < warmup_s) continue;
    ++evaluated;
    if (r.rooms_match >= 0) {
      ++room_scored;
      room_ok += static_cast<std::size_t>(r.rooms_match);
    }
    if (!r.has_fix) continue;
    ++with_fix;
    if (r.est_level != r.truth_level) continue;
    errors.push_back(r.error_m);
    const double det = r.cov_xx * r.cov_yy - r.cov_xy * r.cov_xy;
    if (det > 0.0) {
      const double dx = r.estimate.x - r.truth.x;
      const double dy = r.estimate.y - r.truth.y;
      nees.push_back((r.cov_yy * dx * dx - 2.0 * r.cov_xy * dx * dy + r.cov_xx * dy * dy) / det);
    }
  }
  if (!nees.empty()) {
    constexpr double chi2_2dof_95 = 5.991;
    const auto inside = std::count_if(nees.begin(), nees.end(), [](double v) { return v <= chi2_2dof_95; });
    m.coverage95 = static_cast<double>(inside) / static_cast<double>(nees.size());
    m.nees_median = percentile(nees, 0.5);
  }
  m.samples = evaluated;
  m.availability = evaluated ? static_cast<double>(with_fix) / static_cast<double>(evaluated) : 0.0;
  m.floor_accuracy = with_fix ? static_cast<double>(errors.size()) / static_cast<double>(with_fix) : 0.0;
  if (room_scored) m.room_accuracy = static_cast<double>(room_ok) / static_cast<double>(room_scored);

  // Floor switch latency per true level change.
  double latency_sum = 0.0;
  for (std::size_t i = 1; i < records.size(); ++i) {
    if (records[i].truth_level == records[i - 1].truth_level) continue;
    ++m.level_changes;
    const int target = records[i].truth_level;
    std::size_t j = i;
    while (j < records.size() && records[j].truth_level == target &&
           !(records[j].has_fix && records[j].est_level == target)) {
      ++j;
    }
    const std::size_t end = std::min(j, records.size() - 1);
    latency_sum += records[end].t - records[i].t;
  }
  if (m.level_changes > 0) m.floor_switch_latency_s = latency_sum / m.level_changes;
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
  const auto good = [&](const EstimateRecord& r) {
    return r.has_fix && r.error_m < threshold && r.est_level == r.truth_level;
  };
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (!good(records[i])) continue;
    bool ok = true;
    std::size_t j = i;
    for (; j < records.size() && records[j].t - records[i].t <= hold; ++j) {
      if (!good(records[j])) {
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

void score_room_events(Metrics& m, const std::vector<RoomEvent>& estimated, const std::vector<RoomEvent>& truth,
                       double t_from, double window_s) {
  std::vector<bool> used(estimated.size(), false);
  std::vector<double> latencies;
  int truth_counted = 0, detected = 0;
  for (const auto& tr : truth) {
    const bool counted = tr.t >= t_from;
    truth_counted += counted;
    for (std::size_t k = 0; k < estimated.size(); ++k) {
      const auto& e = estimated[k];
      if (used[k] || e.room != tr.room || e.type != tr.type || std::abs(e.t - tr.t) > window_s) continue;
      used[k] = true;
      if (counted) {
        ++detected;
        latencies.push_back(e.t - tr.t);
      }
      break;
    }
  }
  int est_counted = 0, est_matched = 0;
  for (std::size_t k = 0; k < estimated.size(); ++k) {
    if (estimated[k].t < t_from) continue;
    ++est_counted;
    est_matched += used[k];
  }
  m.room_events = truth_counted;
  m.room_event_recall = truth_counted ? static_cast<double>(detected) / truth_counted : 1.0;
  m.room_event_precision = est_counted ? static_cast<double>(est_matched) / est_counted : 1.0;
  m.room_event_latency_median_s = latencies.empty() ? -1.0 : percentile(latencies, 0.5);
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
  std::vector<RssiReading> scan;
  std::optional<RoomTracker> truth_rooms;
  if (!venue.rooms().empty()) truth_rooms.emplace(venue, truth_room_config());
  std::vector<std::size_t> truth_in, est_in;
  std::size_t i = 0, b = 0, p = 0, g = 0, truth_seen = 0;
  while (g < trace.truth.size()) {
    const double ti = i < trace.imu.size() ? trace.imu[i].t : 1e300;
    const double tb = b < trace.ble.size() ? trace.ble[b].t : 1e300;
    const double tp = p < trace.baro.size() ? trace.baro[p].t : 1e300;
    const double tg = trace.truth[g].t;

    if (ti <= tb && ti <= tp && ti <= tg) {
      const auto start = clock::now();
      engine.on_imu(trace.imu[i++]);
      busy += clock::now() - start;
      ++updates;
    } else if (tb <= tp && tb <= tg) {
      const std::size_t end = scan_end(trace.ble, b);
      if (trace.ble[b].scan != 0) {
        scan.clear();
        for (std::size_t k = b; k < end; ++k) scan.push_back({ble_index[k], trace.ble[k].rssi});
      }
      const auto start = clock::now();
      if (trace.ble[b].scan != 0) {
        engine.on_scan(trace.ble[b].t, scan);
      } else {
        engine.on_ble(trace.ble[b].t, ble_index[b], trace.ble[b].rssi);
      }
      busy += clock::now() - start;
      b = end;
      ++updates;
    } else if (tp <= tg) {
      const auto start = clock::now();
      engine.on_pressure(trace.baro[p].t, trace.baro[p].hpa);
      busy += clock::now() - start;
      ++p;
      ++updates;
    } else {
      const auto& truth = trace.truth[g++];
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

      const auto* pf = engine.particle_filter();
      if (pf && opts.particle_snapshot_every > 0 &&
          truth_seen % static_cast<std::size_t>(opts.particle_snapshot_every) == 0) {
        ParticleSnapshot snap;
        snap.t = truth.t;
        const auto& ps = pf->particles();
        const std::size_t stride =
            std::max<std::size_t>(1, ps.size() / std::max<std::size_t>(1, opts.particle_snapshot_max));
        for (std::size_t k = 0; k < ps.size(); k += stride) {
          snap.particles.push_back({static_cast<float>(ps[k].pos.x), static_cast<float>(ps[k].pos.y),
                                    static_cast<float>(ps[k].weight), static_cast<float>(ps[k].level)});
        }
        result.snapshots.push_back(std::move(snap));
      }
      ++truth_seen;
    }
  }

  result.metrics = compute_metrics(result.records, opts.warmup_s);
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
                            {"compute_us_per_update", m.compute_us_per_update},
                            {"floor_accuracy", m.floor_accuracy},
                            {"floor_switch_latency_s", m.floor_switch_latency_s},
                            {"level_changes", m.level_changes},
                            {"coverage95", m.coverage95},
                            {"nees_median", m.nees_median},
                            {"room_events", m.room_events},
                            {"room_event_precision", m.room_event_precision},
                            {"room_event_recall", m.room_event_recall},
                            {"room_event_latency_median_s", m.room_event_latency_median_s},
                            {"room_accuracy", m.room_accuracy}};
  return j.dump(indent);
}

void write_estimates_csv(const std::string& path, const ReplayResult& result) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fprintf(f, "t,truth_x,truth_y,est_x,est_y,sigma_m,error_m,has_fix,truth_level,est_level,cov_xx,cov_xy,cov_yy\n");
  for (const auto& r : result.records) {
    std::fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%.6g,%.6g,%.6g\n", r.t, r.truth.x, r.truth.y,
                 r.estimate.x, r.estimate.y, r.sigma_m, r.error_m, r.has_fix ? 1 : 0, r.truth_level, r.est_level,
                 r.cov_xx, r.cov_xy, r.cov_yy);
  }
  std::fclose(f);
}

void write_particles_csv(const std::string& path, const ReplayResult& result) {
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  std::fprintf(f, "t,x,y,w,level\n");
  for (const auto& s : result.snapshots) {
    for (const auto& p : s.particles) {
      std::fprintf(f, "%.3f,%.3f,%.3f,%.6g,%d\n", s.t, p[0], p[1], p[2], static_cast<int>(p[3]));
    }
  }
  std::fclose(f);
}

void write_room_events_csv(const std::string& path, const Venue& venue, const ReplayResult& result) {
  struct Row {
    const RoomEvent* event;
    const char* source;
  };
  std::vector<Row> rows;
  for (const auto& e : result.room_events) rows.push_back({&e, "estimate"});
  for (const auto& e : result.truth_room_events) rows.push_back({&e, "truth"});
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.event->t < b.event->t; });

  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write " + path);
  // Names come from map data and may contain commas or quotes.
  const auto quoted = [](const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
  };
  out << "t,source,room_id,room_name,type,probability\n";
  char num[64];
  for (const auto& r : rows) {
    const Room& room = venue.rooms().at(r.event->room);
    std::snprintf(num, sizeof(num), "%.3f", r.event->t);
    out << num << ',' << r.source << ',' << quoted(room.id) << ',' << quoted(room.name) << ','
        << to_string(r.event->type) << ',';
    std::snprintf(num, sizeof(num), "%.3f", r.event->probability);
    out << num << '\n';
  }
}

}  // namespace eval
}  // namespace ipe
