// The particle filter's core: sampling, motion, measurement updates,
// resampling, recovery and the estimate. The beacon-driven proposals are in
// particle_filter_proposal.cpp; the integrity monitor and the map-consistent
// estimate are components of their own.
//
// Adding a measurement type:
//   1. add `on_x(t, ...)`; fill `loglik_` with one log-likelihood per particle
//      (and apply any prediction step first, drawing from `rng_` only);
//   2. call `commit_log_likelihoods(t)`, which weights, normalises (re-seeding
//      if every particle died), resamples and updates the estimate;
//   3. forward it from PositioningEngine and feed it from TracePlayer.

#include "ipe/particle_filter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ipe {

ParticleFilter::ParticleFilter(const Venue& venue, ParticleFilterConfig cfg, RssiModelConfig rssi_cfg,
                               BaroConfig baro_cfg)
    : venue_(&venue),
      cfg_(cfg),
      rssi_model_(venue, rssi_cfg),
      rng_(cfg.seed),
      integrity_(venue.beacons().size(), {cfg.integrity_threshold_db, cfg.integrity_alpha, cfg.integrity_min_samples,
                                          cfg.integrity_max_sigma_m}),
      floor_db_(venue.floor_attenuation_db()),
      multi_level_(venue.multi_level()),
      altimeter_(baro_cfg),
      floor_classifier_(venue, {cfg.floor_prior_k, cfg.floor_prior_eps}),
      fingerprint_map_(venue, {cfg.fingerprint_k, cfg.fingerprint_strongest}),
      level_selector_(cfg.level_smoothing_s, cfg.level_hysteresis) {
  floor_p_.assign(static_cast<std::size_t>(std::max(venue.level_count(), 1)), 1.0);
  cfg_.num_particles = std::max(cfg_.num_particles, 1);
  if (!cfg_.init_proposal && cfg_.init_proposal_auto_density > 0.0) {
    double area = 0.0;
    for (int l = 0; l < venue.level_count(); ++l) area += venue.walkable_area(l);
    cfg_.init_proposal = area > 0.0 && cfg_.num_particles / area < cfg_.init_proposal_auto_density;
  }
  level_weight_.resize(static_cast<std::size_t>(std::max(venue.level_count(), 1)));
  if (cfg_.map_consistent_estimate) map_estimate_.emplace(venue, static_cast<std::size_t>(cfg_.num_particles));
  if (cfg_.closed_region_fraction > 0.0) {
    reachable_.emplace(venue, cfg_.closed_region_fraction, cfg_.closed_region_min_m2);
    // Nothing to avoid: no map, and every draw stays as it was.
    if (!reachable_->any_closed()) reachable_.reset();
  }
  for (int l = 0; l < venue.level_count(); ++l) placeable_total_ += placeable_area(l);
  initialize_uniform();
}

bool ParticleFilter::beacon_excluded(std::size_t beacon_index) const { return integrity_.excluded(beacon_index); }

bool ParticleFilter::check_beacon(std::size_t beacon_index, double rssi) {
  const bool calibrating = cfg_.calibrate_floor_loss && multi_level_;
  if (!cfg_.beacon_integrity && !calibrating) return true;
  // Residuals are only meaningful once the estimate itself can be trusted.
  if (!integrity_.can_judge(cloud_.sigma_radius())) return !cfg_.beacon_integrity || !integrity_.excluded(beacon_index);
  const Beacon& beacon = venue_->beacons()[beacon_index];
  const double expected = expected_rssi(beacon, cloud_.pos, cloud_.level) + mean_rssi_offset_;
  const double residual = rssi - expected;
  const bool admitted = !cfg_.beacon_integrity || integrity_.admit(beacon_index, residual);
  // Selected on the prediction, not the measurement, so the selection does not depend on the noise. The
  // cost: a slab loss that starts far too high predicts every other-floor reading below the threshold and
  // cannot be corrected (experiment 10).
  if (admitted && calibrating && expected >= cfg_.loss_calibration_min_rssi_dbm) {
    calibrate_floor_loss(beacon, residual);
  }
  return admitted;
}

void ParticleFilter::calibrate_floor_loss(const Beacon& beacon, double residual) {
  // The model subtracts floor_db_ per slab; a reading stronger than predicted (r > 0) means less loss.
  const int slabs = std::abs(beacon.level - cloud_.level);
  if (slabs == 0) return;
  const double r = std::clamp(residual, -cfg_.loss_calibration_clip_db, cfg_.loss_calibration_clip_db);
  floor_db_ = std::clamp(floor_db_ - cfg_.loss_calibration_rate * r / slabs, 0.0, kMaxFloorDb);
}

Vec2 ParticleFilter::sample_position(int level, int open_level) {
  // Rejection sampling over the bounds (or the level footprint's box); without
  // a footprint or closed regions the first draw is always accepted.
  const Bounds b = level >= 0 ? venue_->sample_box(level) : venue_->bounds();
  const int closed_level = open_level >= 0 ? open_level : std::max(level, 0);
  const auto ok = [&](Vec2 q) {
    return venue_->inside(q, level) && (!reachable_ || reachable_->open(q, closed_level));
  };
  Vec2 pos{rng_.uniform(b.min_x, b.max_x), rng_.uniform(b.min_y, b.max_y)};
  for (int i = 1; i < kFootprintTries && !ok(pos); ++i) {
    pos = {rng_.uniform(b.min_x, b.max_x), rng_.uniform(b.min_y, b.max_y)};
  }
  return pos;
}

double ParticleFilter::placeable_area(int level) const {
  return venue_->walkable_area(level) - (reachable_ ? reachable_->closed_area(level) : 0.0);
}

int ParticleFilter::sample_level_by_area() {
  double total = 0.0;
  for (int l = 0; l < venue_->level_count(); ++l) total += placeable_area(l);
  double u = rng_.uniform() * total;
  for (int l = 0; l < venue_->level_count(); ++l) {
    u -= placeable_area(l);
    if (u < 0.0) return l;
  }
  return venue_->level_count() - 1;
}

Particle ParticleFilter::sample_uniform_particle() {
  Particle p;
  p.pos = sample_position();
  p.heading = rng_.uniform(-std::numbers::pi, std::numbers::pi);
  p.step_scale = std::max(0.5, rng_.normal(1.0, cfg_.step_scale_sigma));
  p.rssi_offset = cfg_.estimate_rssi_offset ? rng_.normal(0.0, cfg_.rssi_offset_sigma) : 0.0;
  if (multi_level_ && (venue_->has_level_footprints() || reachable_)) {
    // Levels of different size (own footprints, or closed regions): uniform
    // over all walkable area, so a small landing gets few particles.
    p.level = sample_level_by_area();
    p.pos = sample_position(p.level);
  } else if (multi_level_) {
    p.level = std::min(static_cast<int>(rng_.uniform() * venue_->level_count()), venue_->level_count() - 1);
  }
  if (multi_level_) {
    // Absolute altitude is unknown, so a fresh hypothesis is made consistent with the current reading.
    if (baro_ready_) {
      p.baro_offset = fresh_baro_offset(p.level);
    }
  }
  return p;
}

void ParticleFilter::maybe_change_level(Particle& p, double prob) {
  const Connector* c = venue_->connector_at(p.pos, p.level);
  if (!c || !rng_.bernoulli(prob)) return;
  // Connector levels are sorted; move to a neighbour in that list.
  const auto it = std::find(c->levels.begin(), c->levels.end(), p.level);
  const bool has_up = it + 1 != c->levels.end();
  const bool has_down = it != c->levels.begin();
  const bool up = has_up && (!has_down || rng_.bernoulli(0.5));
  p.level = up ? *(it + 1) : *(it - 1);
}

double ParticleFilter::fresh_baro_offset(int level, bool jitter) {
  const double noise = jitter ? rng_.normal(0.0, cfg_.baro_offset_sigma_m) : 0.0;
  if (baro_reference_valid_ && cfg_.baro_reseed_keep_reference > 0.0 &&
      rng_.bernoulli(cfg_.baro_reseed_keep_reference)) {
    return baro_reference_ + noise;
  }
  return altimeter_.altitude() - elevation(level) + noise;
}

void ParticleFilter::update_baro_drift(double t, double alt) {
  double sum_w = 0.0, sum_r = 0.0;
  for (const auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    sum_w += p.weight;
    sum_r += p.weight * (alt - p.baro_offset - elevation(p.level));
  }
  const double r = sum_w > 0.0 ? sum_r / sum_w : 0.0;
  const int level = cloud_.level;
  const int dir = r > 0.0 ? 1 : -1;
  const int next = level + dir;
  const bool drifting =
      next >= 0 && next < venue_->level_count() && std::abs(r) > 0.5 * std::abs(elevation(next) - elevation(level));
  if (!drifting) {
    baro_drift_dir_ = 0;
    baro_jump_armed_ = false;
    return;
  }
  if (dir != baro_drift_dir_) {
    baro_drift_dir_ = dir;
    baro_drift_since_ = t;
  }
  baro_jump_armed_ = t - baro_drift_since_ >= cfg_.baro_jump_hold_s;
}

void ParticleFilter::maybe_baro_jump(Particle& p, double alt) {
  const double r = alt - p.baro_offset - elevation(p.level);
  const int to = p.level + (r > 0.0 ? 1 : -1);
  if (to < 0 || to >= venue_->level_count()) return;
  // Only when the particle's own residual fits the other level better.
  if (std::abs(r - (elevation(to) - elevation(p.level))) >= std::abs(r)) return;
  if (!rng_.bernoulli(cfg_.baro_jump_prob)) return;
  const Connector* c = venue_->nearest_connector(p.pos, p.level, to, cfg_.baro_jump_radius_m);
  if (!c || c->area.empty()) return;
  // A point of the connector that is walkable on the new level: rejection-sample its bounding box.
  Bounds box{c->area[0].x, c->area[0].y, c->area[0].x, c->area[0].y};
  for (const Vec2 v : c->area) {
    box.min_x = std::min(box.min_x, v.x);
    box.min_y = std::min(box.min_y, v.y);
    box.max_x = std::max(box.max_x, v.x);
    box.max_y = std::max(box.max_y, v.y);
  }
  for (int i = 0; i < kFootprintTries; ++i) {
    const Vec2 q{rng_.uniform(box.min_x, box.max_x), rng_.uniform(box.min_y, box.max_y)};
    if (c->contains(q) && venue_->inside(q, to)) {
      p.pos = q;
      p.level = to;
      return;
    }
  }
}

void ParticleFilter::initialize_uniform() {
  const auto n = static_cast<std::size_t>(cfg_.num_particles);
  particles_.resize(n);
  for (auto& p : particles_) {
    p = sample_uniform_particle();
    p.weight = 1.0 / static_cast<double>(n);
  }
  update_estimate(0.0);
}

void ParticleFilter::on_step(const StepEvent& step) {
  now_ = step.t;
  const double heading_sigma = deg2rad(cfg_.heading_sigma_deg);
  // The step's own motion: the weighted mean move of the particles that survive it, on the reported level
  // (all of them if none there), by their weights before the step. What else the step does to the estimate
  // (particles dying at walls, resampling) is a correction, not motion. Accumulated only; no draws.
  Vec2 move_level{}, move_all{};
  double w_level = 0.0, w_all = 0.0;
  for (auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    p.heading = wrap_angle(p.heading + step.heading_delta + rng_.normal(0.0, heading_sigma));
    p.step_scale = std::clamp(p.step_scale + rng_.normal(0.0, cfg_.step_scale_walk), 0.5, 1.5);
    const double len = std::max(0.0, step.length * p.step_scale + rng_.normal(0.0, cfg_.step_length_sigma));
    const Vec2 next = p.pos + Vec2{std::cos(p.heading), std::sin(p.heading)} * len;

    if (!venue_->inside(next, p.level) || venue_->crosses_wall(p.pos, next, p.level)) {
      p.weight = 0.0;
      continue;
    }
    const Vec2 move = (next - p.pos) * p.weight;
    move_all += move;
    w_all += p.weight;
    if (p.level == estimate_.level) {
      move_level += move;
      w_level += p.weight;
    }
    p.pos = next;
    if (multi_level_) maybe_change_level(p, cfg_.level_change_prob_step);
  }
  step_motion_ = w_level > 0.0 ? move_level * (1.0 / w_level) : w_all > 0.0 ? move_all * (1.0 / w_all) : Vec2{};
  finish_update(step.t);
}

void ParticleFilter::on_heading(double t, double heading, double sigma) {
  now_ = t;
  loglik_.resize(particles_.size());
  const double eps = cfg_.heading_outlier_prob;
  const double norm = 1.0 / (sigma * std::sqrt(2.0 * std::numbers::pi));
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    const double z = wrap_angle(heading - particles_[i].heading) / sigma;
    loglik_[i] = std::log((1.0 - eps) * norm * std::exp(-0.5 * z * z) + eps / (2.0 * std::numbers::pi));
  }
  commit_log_likelihoods(t);
}

void ParticleFilter::on_rssi(double t, std::size_t beacon_index, double rssi) {
  if (beacon_index >= venue_->beacons().size() || !venue_->beacons()[beacon_index].modelled) return;
  now_ = t;
  if (!check_beacon(beacon_index, rssi)) return;
  remember_packet(t, beacon_index, rssi);
  if (cfg_.init_proposal && !radio_seeded_) {
    // Collect packets for a short window, seed from the one with the shortest
    // apparent range (tightest ring, fewest walls in the way), then apply all
    // of them so none of the measurements is lost.
    const bool room = pending_count_ < pending_.size();
    if (room) pending_[pending_count_++] = {t, beacon_index, rssi};
    if (room && pending_count_ < pending_.size() && t - pending_[0].t < cfg_.init_window_s) return;
    const auto best = std::min_element(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pending_count_),
                                       [&](const Packet& a, const Packet& b) {
                                         return apparent_range(a.beacon, a.rssi) < apparent_range(b.beacon, b.rssi);
                                       });
    seed_from_beacon(best->beacon, best->rssi);
    flush_pending();
    if (!room) update_rssi(t, beacon_index, rssi);
    return;
  }
  update_rssi(t, beacon_index, rssi);
}

void ParticleFilter::on_rssi_scan(double t, std::span<const RssiReading> readings) {
  now_ = t;
  scan_.clear();
  for (const auto& r : readings) {
    if (r.beacon_index >= venue_->beacons().size() || !venue_->beacons()[r.beacon_index].modelled) continue;
    if (!check_beacon(r.beacon_index, r.rssi)) continue;
    remember_packet(t, r.beacon_index, r.rssi);
    scan_.push_back(r);
  }
  // The fingerprints compare the scan as it was heard: every beacon, also those without a model and
  // those the integrity monitor distrusts (a moved beacon is in the training scans where it is now).
  const int found = cfg_.fingerprint_weight > 0.0 ? fingerprint_map_.nearest(readings, neighbours_) : 0;
  if (found > 0) {
    neighbour_count_ = found;
    last_fingerprint_t_ = t;
  }
  if (scan_.empty() && found == 0) return;
  if (found > 0 && cfg_.fingerprint_proposal && !radio_seeded_) {
    seed_from_fingerprints();
    flush_pending();
  } else if (cfg_.init_proposal && !radio_seeded_ && !scan_.empty()) {
    // A scan is already a batch, so no collection window: seed from the
    // shortest apparent range among its readings and any lone packets waiting.
    const auto best_in_scan = std::min_element(scan_.begin(), scan_.end(), [&](const auto& a, const auto& b) {
      return apparent_range(a.beacon_index, a.rssi) < apparent_range(b.beacon_index, b.rssi);
    });
    std::size_t beacon = best_in_scan->beacon_index;
    double rssi = best_in_scan->rssi;
    for (std::size_t i = 0; i < pending_count_; ++i) {
      if (apparent_range(pending_[i].beacon, pending_[i].rssi) < apparent_range(beacon, rssi)) {
        beacon = pending_[i].beacon;
        rssi = pending_[i].rssi;
      }
    }
    seed_from_beacon(beacon, rssi);
    flush_pending();
  }
  // Like the fingerprint neighbours, the floor vote compares the scan as it was heard.
  floor_prior_active_ = multi_level_ && cfg_.floor_prior_weight > 0.0 && floor_classifier_.classify(readings, floor_p_);
  if (floor_prior_active_) last_floor_t_ = t;
  fingerprint_active_ = found > 0;
  update_rssi(t, scan_);
  floor_prior_active_ = false;
  fingerprint_active_ = false;
}

void ParticleFilter::update_rssi(double t, std::size_t beacon_index, double rssi) {
  const RssiReading reading{beacon_index, rssi};
  update_rssi(t, std::span<const RssiReading>(&reading, 1));
}

void ParticleFilter::update_rssi(double t, std::span<const RssiReading> readings) {
  loglik_.resize(particles_.size());
  double max_ll = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    auto& p = particles_[i];
    if (cfg_.estimate_rssi_offset) p.rssi_offset += rng_.normal(0.0, cfg_.rssi_offset_walk);
    if (p.weight <= 0.0) {
      loglik_[i] = -std::numeric_limits<double>::infinity();
      continue;
    }
    double ll = 0.0;
    for (const auto& r : readings) {
      const Beacon& beacon = venue_->beacons()[r.beacon_index];
      const double expected = expected_rssi(beacon, p.pos, p.level) + p.rssi_offset;
      ll += rssi_model_.log_likelihood(r.rssi - expected);
    }
    if (fingerprint_active_) {
      ll *= cfg_.fingerprint_model_weight;
      const double eps = std::clamp(cfg_.fingerprint_eps, 1e-9, 1.0);
      ll += cfg_.fingerprint_weight *
            std::log(eps / placeable_total_ + (1.0 - eps) * fingerprint_density(p.pos, p.level));
    }
    // Floor classifier output for this scan, as a likelihood on the particle's level. It is part of the
    // recovery statistic, so a cloud on a floor the classifier rejects draws injections.
    if (floor_prior_active_) ll += cfg_.floor_prior_weight * std::log(floor_p_[static_cast<std::size_t>(p.level)]);
    loglik_[i] = ll;
    max_ll = std::max(max_ll, loglik_[i]);
  }

  if (cfg_.recovery && std::isfinite(max_ll)) {
    double avg = 0.0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
      if (particles_[i].weight > 0.0) avg += particles_[i].weight * std::exp(loglik_[i] - max_ll);
    }
    const double w_avg = avg * std::exp(max_ll);
    if (w_slow_ == 0.0) w_slow_ = w_fast_ = w_avg;
    w_slow_ += cfg_.recovery_alpha_slow * (w_avg - w_slow_);
    w_fast_ += cfg_.recovery_alpha_fast * (w_avg - w_fast_);
  }

  localized_ = true;
  commit_log_likelihoods(t);
}

void ParticleFilter::on_pressure(double t, double hpa) {
  now_ = t;
  const double alt = altimeter_.process(t, hpa);
  if (!multi_level_) return;
  if (!baro_ready_) {
    for (auto& p : particles_) p.baro_offset = alt - elevation(p.level) + rng_.normal(0.0, cfg_.baro_offset_sigma_m);
    baro_ready_ = true;
    last_baro_update_t_ = t;
    return;
  }
  if (t - last_baro_update_t_ < cfg_.baro_update_interval_s) return;
  last_baro_update_t_ = t;

  // Prediction: lift rides produce no steps, so connector transitions are
  // also proposed here; the offset random walk absorbs weather drift.
  if (cfg_.baro_jump) update_baro_drift(t, alt);
  for (auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    maybe_change_level(p, cfg_.level_change_prob_baro);
    if (baro_jump_armed_) maybe_baro_jump(p, alt);
    p.baro_offset += rng_.normal(0.0, cfg_.baro_offset_walk_m);
  }

  loglik_.resize(particles_.size());
  const double s = cfg_.baro_sigma_m;
  const double norm = 1.0 / (s * std::sqrt(2.0 * std::numbers::pi));
  const double outlier = cfg_.baro_outlier_prob / 20.0;  // uniform over a 20 m altitude span
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    const auto& p = particles_[i];
    const double z = (alt - p.baro_offset - elevation(p.level)) / s;
    loglik_[i] = std::log((1.0 - cfg_.baro_outlier_prob) * norm * std::exp(-0.5 * z * z) + outlier);
  }
  commit_log_likelihoods(t);
}

void ParticleFilter::commit_log_likelihoods(double t) {
  apply_log_likelihoods(loglik_);
  finish_update(t);
}

void ParticleFilter::finish_update(double t) {
  normalize_or_recover();
  resample_if_needed();
  update_estimate(t);
}

bool ParticleFilter::apply_log_likelihoods(const std::vector<double>& loglik) {
  double max_ll = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    if (particles_[i].weight > 0.0) max_ll = std::max(max_ll, loglik[i]);
  }
  if (!std::isfinite(max_ll)) return false;
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    particles_[i].weight *= std::exp(loglik[i] - max_ll);
  }
  return true;
}

void ParticleFilter::normalize_or_recover() {
  double sum = 0.0;
  for (const auto& p : particles_) sum += p.weight;

  if (sum <= 0.0 || !std::isfinite(sum)) {
    // Every hypothesis was invalidated (e.g. the user walked through a door
    // that is missing from the map). Re-seed around the last estimate.
    ++recovery_count_;
    const double n = static_cast<double>(particles_.size());
    for (auto& p : particles_) {
      Particle fresh = sample_uniform_particle();
      if (cloud_.valid) {
        Vec2 pos = cloud_.pos + Vec2{rng_.normal(0.0, 3.0), rng_.normal(0.0, 3.0)};
        const int level = multi_level_ ? cloud_.level : 0;
        if (reachable_) {
          // Near the estimate, not anywhere on the floor: a draw in a closed room is drawn again. The
          // re-seed still crosses walls, since a door missing from the map is what got the cloud here.
          for (int i = 1; i < kFootprintTries && !placeable(pos, level); ++i) {
            pos = cloud_.pos + Vec2{rng_.normal(0.0, 3.0), rng_.normal(0.0, 3.0)};
          }
        }
        if (placeable(pos, level)) {
          fresh.pos = pos;
        } else if (multi_level_ && (venue_->has_level_footprints() || reachable_)) {
          fresh.pos = sample_position(level);
        }
        fresh.heading = wrap_angle(cloud_.heading + rng_.normal(0.0, 0.5));
        if (multi_level_) {
          fresh.level = cloud_.level;
          if (baro_ready_) fresh.baro_offset = fresh_baro_offset(fresh.level, false);
        }
      }
      p = fresh;
      p.weight = 1.0 / n;
    }
    return;
  }
  for (auto& p : particles_) p.weight /= sum;
}

double ParticleFilter::effective_sample_size() const {
  double sum_sq = 0.0;
  for (const auto& p : particles_) sum_sq += p.weight * p.weight;
  return sum_sq > 0.0 ? 1.0 / sum_sq : 0.0;
}

void ParticleFilter::resample_if_needed() {
  const auto n = particles_.size();
  if (effective_sample_size() >= cfg_.resample_ess_ratio * static_cast<double>(n)) return;
  ++resample_count_;

  // Systematic resampling: O(N) with a single random offset.
  scratch_.resize(n);
  const double step = 1.0 / static_cast<double>(n);
  double u = rng_.uniform() * step;
  double cumulative = particles_[0].weight;
  std::size_t j = 0;
  for (std::size_t i = 0; i < n; ++i) {
    while (u > cumulative && j + 1 < n) cumulative += particles_[++j].weight;
    scratch_[i] = particles_[j];
    u += step;
  }

  double inject_fraction = 0.0;
  if (cfg_.recovery && w_slow_ > 0.0) {
    inject_fraction = std::clamp(1.0 - w_fast_ / w_slow_, 0.0, cfg_.recovery_max_fraction);
  }

  for (auto& p : scratch_) {
    if (inject_fraction > 0.0 && rng_.bernoulli(inject_fraction)) {
      p = sample_recovery_particle();
    } else {
      const Vec2 jittered = p.pos + Vec2{rng_.normal(0.0, cfg_.roughening_m), rng_.normal(0.0, cfg_.roughening_m)};
      if (venue_->inside(jittered, p.level) && !venue_->crosses_wall(p.pos, jittered, p.level)) p.pos = jittered;
    }
    p.weight = step;
  }
  particles_.swap(scratch_);
}

ParticleFilter::Moments ParticleFilter::moments(int level) const {
  const double level_w = multi_level_ ? level_weight_[static_cast<std::size_t>(level)] : 1.0;
  const auto counts = [&](const Particle& p) { return !multi_level_ || p.level == level; };
  Moments m;
  double sx = 0.0, sy = 0.0;
  for (const auto& p : particles_) {
    if (!counts(p)) continue;
    sx += p.weight * p.pos.x;
    sy += p.weight * p.pos.y;
    m.sin_heading += p.weight * std::sin(p.heading);
    m.cos_heading += p.weight * std::cos(p.heading);
    m.rssi_offset += p.weight * p.rssi_offset;
    m.baro_offset += p.weight * p.baro_offset;
  }
  if (multi_level_ && level_w > 0.0) {
    sx /= level_w;
    sy /= level_w;
    m.rssi_offset /= level_w;
    m.baro_offset /= level_w;
  }
  m.mean = {sx, sy};
  for (const auto& p : particles_) {
    if (!counts(p)) continue;
    const double dx = p.pos.x - sx;
    const double dy = p.pos.y - sy;
    m.cxx += p.weight * dx * dx;
    m.cxy += p.weight * dx * dy;
    m.cyy += p.weight * dy * dy;
  }
  if (multi_level_ && level_w > 0.0) {
    m.cxx /= level_w;
    m.cxy /= level_w;
    m.cyy /= level_w;
  }
  return m;
}

void ParticleFilter::update_estimate(double t) {
  int top = 0;
  if (multi_level_) {
    // The most likely level is the weighted mode; position statistics use only
    // the particles on one level so a split cloud does not average floors.
    std::fill(level_weight_.begin(), level_weight_.end(), 0.0);
    for (const auto& p : particles_) level_weight_[static_cast<std::size_t>(p.level)] += p.weight;
    top = static_cast<int>(std::max_element(level_weight_.begin(), level_weight_.end()) - level_weight_.begin());
  }
  const auto to_estimate = [&](const Moments& m, int level) {
    Estimate e;
    e.t = t;
    e.pos = m.mean;
    e.heading = std::atan2(m.sin_heading, m.cos_heading);
    e.cov_xx = m.cxx;
    e.cov_xy = m.cxy;
    e.cov_yy = m.cyy;
    e.level = level;
    e.z = multi_level_ ? elevation(level) : 0.0;
    e.valid = localized_;
    return e;
  };

  const Moments cloud = moments(top);
  mean_rssi_offset_ = cloud.rssi_offset;
  const double top_w = multi_level_ ? level_weight_[static_cast<std::size_t>(top)] : 1.0;
  if (multi_level_ && baro_ready_ && localized_ && top_w > 0.0) {
    baro_reference_ = cloud.baro_offset;
    baro_reference_valid_ = true;
  }
  cloud_ = to_estimate(cloud, top);

  // The estimate is not a position until radio has been applied, so neither is its level history.
  if (!localized_) level_selector_.reset();
  const int level = multi_level_ ? level_selector_.update(t, level_weight_) : 0;
  const Moments shown = level == top ? cloud : moments(level);
  estimate_ = to_estimate(shown, level);
  const Vec2 pos =
      map_estimate_ && localized_ ? map_estimate_->position(particles_, shown.mean, level, multi_level_) : shown.mean;
  // Second moments about the reported point, so a mode estimate keeps the spread of the whole cloud.
  const Vec2 off = shown.mean - pos;
  estimate_.pos = pos;
  estimate_.cov_xx += off.x * off.x;
  estimate_.cov_xy += off.x * off.y;
  estimate_.cov_yy += off.y * off.y;
}

}  // namespace ipe
