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
      multi_level_(venue.multi_level()),
      altimeter_(baro_cfg) {
  cfg_.num_particles = std::max(cfg_.num_particles, 1);
  health_.resize(venue.beacons().size());
  level_weight_.resize(static_cast<std::size_t>(std::max(venue.level_count(), 1)));
  initialize_uniform();
}

bool ParticleFilter::beacon_excluded(std::size_t beacon_index) const {
  return beacon_index < health_.size() && health_[beacon_index].excluded;
}

bool ParticleFilter::check_beacon(std::size_t beacon_index, double rssi) {
  auto& h = health_[beacon_index];
  if (!cfg_.beacon_integrity) return true;
  // Residuals are only meaningful once the estimate itself can be trusted.
  if (estimate_.sigma_radius() > cfg_.integrity_max_sigma_m) return !h.excluded;

  const Beacon& beacon = venue_->beacons()[beacon_index];
  const double residual =
      rssi - (rssi_model_.expected_rssi(beacon, estimate_.pos, estimate_.level) + mean_rssi_offset_);
  h.mean_residual = h.samples == 0 ? residual : h.mean_residual + cfg_.integrity_alpha * (residual - h.mean_residual);
  ++h.samples;

  if (!h.excluded && h.samples >= cfg_.integrity_min_samples &&
      std::abs(h.mean_residual) > cfg_.integrity_threshold_db) {
    // Never drop more than a third of the infrastructure on our own say-so.
    const auto excluded =
        std::count_if(health_.begin(), health_.end(), [](const BeaconHealth& b) { return b.excluded; });
    if (static_cast<std::size_t>(excluded + 1) * 3 <= health_.size()) h.excluded = true;
  } else if (h.excluded && std::abs(h.mean_residual) < 0.5 * cfg_.integrity_threshold_db) {
    h.excluded = false;
  }
  return !h.excluded;
}

Vec2 ParticleFilter::sample_position() {
  // Rejection sampling over the bounds; without a footprint the first draw is always accepted.
  const auto& b = venue_->bounds();
  Vec2 pos{rng_.uniform(b.min_x, b.max_x), rng_.uniform(b.min_y, b.max_y)};
  for (int i = 1; i < kFootprintTries && !venue_->inside(pos); ++i) {
    pos = {rng_.uniform(b.min_x, b.max_x), rng_.uniform(b.min_y, b.max_y)};
  }
  return pos;
}

Particle ParticleFilter::sample_uniform_particle() {
  Particle p;
  p.pos = sample_position();
  p.heading = rng_.uniform(-std::numbers::pi, std::numbers::pi);
  p.step_scale = std::max(0.5, rng_.normal(1.0, cfg_.step_scale_sigma));
  p.rssi_offset = cfg_.estimate_rssi_offset ? rng_.normal(0.0, cfg_.rssi_offset_sigma) : 0.0;
  if (multi_level_) {
    p.level = std::min(static_cast<int>(rng_.uniform() * venue_->level_count()), venue_->level_count() - 1);
    // Absolute altitude is unknown, so a fresh hypothesis is made consistent with the current reading.
    if (baro_ready_) {
      p.baro_offset = altimeter_.altitude() - elevation(p.level) + rng_.normal(0.0, cfg_.baro_offset_sigma_m);
    }
  }
  return p;
}

double ParticleFilter::ring_density(double d, double log10_mean, double log10_sigma) {
  d = std::max(d, 1e-9);
  const double z = (std::log10(d) - log10_mean) / log10_sigma;
  const double per_decade = std::exp(-0.5 * z * z) / (log10_sigma * std::sqrt(2.0 * std::numbers::pi));
  // d(log10 r) = dr / (r ln 10); the bearing spreads it over a circle of length 2 pi r.
  return per_decade / (d * std::numbers::ln10 * 2.0 * std::numbers::pi * d);
}

ParticleFilter::Ring ParticleFilter::ring_for(const Beacon& beacon, double rssi, double rssi_offset) const {
  // Inverts the free-space model for a device reading `rssi_offset` dB high.
  // Walls and path-loss mismatch make the true range differ by more than the
  // RSSI noise, so the ring is widened; the importance weights stay exact.
  const double slope = 10.0 * beacon.path_loss_exp;
  const double spread_db = rssi_model_.config().sigma_db * cfg_.proposal_ring_scale;
  return {(beacon.tx_power_dbm + rssi_offset - rssi) / slope, std::max(spread_db / slope, 1e-3)};
}

double ParticleFilter::ring_level_prob(int beacon_level, int level) const {
  if (!multi_level_) return 1.0;
  const bool has_up = beacon_level + 1 < venue_->level_count();
  const bool has_down = beacon_level > 0;
  const int neighbours = (has_up ? 1 : 0) + (has_down ? 1 : 0);
  if (neighbours == 0) return level == beacon_level ? 1.0 : 0.0;
  if (level == beacon_level) return 1.0 - cfg_.proposal_level_jump;
  if ((level == beacon_level + 1 && has_up) || (level == beacon_level - 1 && has_down)) {
    return cfg_.proposal_level_jump / neighbours;
  }
  return 0.0;
}

Particle ParticleFilter::sample_ring_particle(const Beacon& beacon, double rssi, RingDraw& draw) {
  Particle p;
  p.heading = rng_.uniform(-std::numbers::pi, std::numbers::pi);
  p.step_scale = std::max(0.5, rng_.normal(1.0, cfg_.step_scale_sigma));
  // Offset and position are drawn jointly and rejected together, so the
  // accepted pairs follow the ring truncated to the venue with a single
  // normaliser (the acceptance rate) instead of one per offset.
  draw = {};
  bool inside = false;
  while (!inside && draw.attempts < kRingTries) {
    ++draw.attempts;
    p.rssi_offset = cfg_.estimate_rssi_offset ? rng_.normal(0.0, cfg_.rssi_offset_sigma) : 0.0;
    const Ring ring = ring_for(beacon, rssi, p.rssi_offset);
    const double range = std::pow(10.0, ring.log10_mean + ring.log10_sigma * rng_.normal());
    const double bearing = rng_.uniform(-std::numbers::pi, std::numbers::pi);
    p.pos = beacon.pos + Vec2{std::cos(bearing), std::sin(bearing)} * range;
    inside = venue_->inside(p.pos);
  }
  if (!inside) {
    // The last offset tried is biased towards rings that miss the venue.
    draw.fell_back = true;
    p.pos = sample_position();
    p.rssi_offset = cfg_.estimate_rssi_offset ? rng_.normal(0.0, cfg_.rssi_offset_sigma) : 0.0;
  }
  if (multi_level_) {
    p.level = beacon.level;
    if (rng_.bernoulli(cfg_.proposal_level_jump)) {
      const bool has_up = beacon.level + 1 < venue_->level_count();
      const bool has_down = beacon.level > 0;
      if (has_up && has_down) {
        p.level += rng_.bernoulli(0.5) ? 1 : -1;
      } else if (has_up) {
        p.level += 1;
      } else if (has_down) {
        p.level -= 1;
      }
    }
    if (baro_ready_) {
      p.baro_offset = altimeter_.altitude() - elevation(p.level) + rng_.normal(0.0, cfg_.baro_offset_sigma_m);
    }
  }
  return p;
}

void ParticleFilter::seed_from_beacon(std::size_t beacon_index, double rssi) {
  if (beacon_index >= venue_->beacons().size()) return;
  const Beacon& beacon = venue_->beacons()[beacon_index];
  const double f = std::clamp(cfg_.init_proposal_fraction, 0.0, 1.0);

  std::size_t attempts = 0, accepted = 0;
  for (auto& p : particles_) {
    if (rng_.bernoulli(f)) {
      RingDraw draw;
      p = sample_ring_particle(beacon, rssi, draw);
      attempts += static_cast<std::size_t>(draw.attempts);
      if (!draw.fell_back) ++accepted;
    } else {
      p = sample_uniform_particle();
    }
  }

  // Importance weight = prior / proposal. Heading and step scale have the
  // prior distribution in both. With acceptance rate Z per try and K tries, a
  // ring draw has density ring(x | offset) * prior(offset) * (1 - (1-Z)^K) / Z
  // inside the venue, plus (1-Z)^K spread uniformly for the fallbacks; the
  // offset prior cancels against the prior's.
  const double area = venue_->walkable_area();
  const double levels = multi_level_ ? static_cast<double>(venue_->level_count()) : 1.0;
  const double z = attempts > 0 ? static_cast<double>(accepted) / static_cast<double>(attempts) : 1.0;
  const double miss_all = std::pow(1.0 - z, kRingTries);
  const double ring_scale = z > 0.0 ? (1.0 - miss_all) / z : static_cast<double>(kRingTries);
  double sum = 0.0;
  for (auto& p : particles_) {
    const Ring ring = ring_for(beacon, rssi, p.rssi_offset);
    const double ring_pos =
        area * ring_scale * ring_density(distance(p.pos, beacon.pos), ring.log10_mean, ring.log10_sigma) + miss_all;
    p.weight = 1.0 / ((1.0 - f) + f * levels * ring_level_prob(beacon.level, p.level) * ring_pos);
    sum += p.weight;
  }
  for (auto& p : particles_) p.weight /= sum;
  radio_seeded_ = true;
  update_estimate(now_);
}

void ParticleFilter::remember_packet(double t, std::size_t beacon_index, double rssi) {
  const auto capacity = static_cast<std::size_t>(std::clamp(cfg_.proposal_history, 1, kMaxProposalHistory));
  history_[history_head_] = {t, beacon_index, rssi};
  history_head_ = (history_head_ + 1) % capacity;
  history_count_ = std::min(history_count_ + 1, capacity);
}

Particle ParticleFilter::sample_recovery_particle() {
  if (!cfg_.recovery_proposal) return sample_uniform_particle();
  std::array<std::size_t, kMaxProposalHistory> fresh{};
  std::size_t n = 0;
  for (std::size_t i = 0; i < history_count_; ++i) {
    if (now_ - history_[i].t <= cfg_.proposal_max_age_s) fresh[n++] = i;
  }
  if (n == 0) return sample_uniform_particle();
  const auto pick = std::min(static_cast<std::size_t>(rng_.uniform() * static_cast<double>(n)), n - 1);
  const Packet& packet = history_[fresh[pick]];
  RingDraw draw;
  return sample_ring_particle(venue_->beacons()[packet.beacon], packet.rssi, draw);
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
  for (auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    p.heading = wrap_angle(p.heading + step.heading_delta + rng_.normal(0.0, heading_sigma));
    p.step_scale = std::clamp(p.step_scale + rng_.normal(0.0, cfg_.step_scale_walk), 0.5, 1.5);
    const double len = std::max(0.0, step.length * p.step_scale + rng_.normal(0.0, cfg_.step_length_sigma));
    const Vec2 next = p.pos + Vec2{std::cos(p.heading), std::sin(p.heading)} * len;

    if (!venue_->inside(next) || (cfg_.wall_constraint && venue_->crosses_wall(p.pos, next, p.level))) {
      p.weight = 0.0;
      continue;
    }
    p.pos = next;
    if (multi_level_) maybe_change_level(p, cfg_.level_change_prob_step);
  }
  normalize_or_recover();
  resample_if_needed();
  update_estimate(step.t);
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
  apply_log_likelihoods(loglik_);
  normalize_or_recover();
  resample_if_needed();
  update_estimate(t);
}

void ParticleFilter::on_rssi(double t, std::size_t beacon_index, double rssi) {
  if (beacon_index >= venue_->beacons().size()) return;
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
    if (r.beacon_index >= venue_->beacons().size() || !check_beacon(r.beacon_index, r.rssi)) continue;
    remember_packet(t, r.beacon_index, r.rssi);
    scan_.push_back(r);
  }
  if (scan_.empty()) return;
  if (cfg_.init_proposal && !radio_seeded_) {
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
  update_rssi(t, scan_);
}

double ParticleFilter::apparent_range(std::size_t beacon_index, double rssi) const {
  const Beacon& b = venue_->beacons()[beacon_index];
  return (b.tx_power_dbm - rssi) / b.path_loss_exp;
}

void ParticleFilter::flush_pending() {
  for (std::size_t i = 0; i < pending_count_; ++i) update_rssi(pending_[i].t, pending_[i].beacon, pending_[i].rssi);
  pending_count_ = 0;
}

void ParticleFilter::update_rssi(double t, std::size_t beacon_index, double rssi) {
  const RssiReading reading{beacon_index, rssi};
  update_rssi(t, std::span<const RssiReading>(&reading, 1));
}

void ParticleFilter::update_rssi(double t, std::span<const RssiReading> readings) {
  // Readings of one scan share most of their model error, so their summed
  // log-likelihood is tempered by k^-gamma (gamma = 1: the scan counts once).
  const double temper =
      cfg_.scan_tempering > 0.0 ? std::pow(static_cast<double>(readings.size()), -cfg_.scan_tempering) : 1.0;

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
      const double expected = rssi_model_.expected_rssi(beacon, p.pos, p.level) + p.rssi_offset;
      ll += rssi_model_.log_likelihood(r.rssi - expected);
    }
    loglik_[i] = temper * ll;
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

  apply_log_likelihoods(loglik_);
  normalize_or_recover();
  resample_if_needed();
  update_estimate(t);
}

void ParticleFilter::on_pressure(double t, double hpa) {
  now_ = t;
  const double alt = altimeter_.process(t, hpa);
  if (!multi_level_ || !cfg_.use_barometer) return;
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
  for (auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    maybe_change_level(p, cfg_.level_change_prob_baro);
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
  apply_log_likelihoods(loglik_);
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
      if (estimate_.valid) {
        Vec2 pos = estimate_.pos + Vec2{rng_.normal(0.0, 3.0), rng_.normal(0.0, 3.0)};
        if (venue_->inside(pos)) fresh.pos = pos;
        fresh.heading = wrap_angle(estimate_.heading + rng_.normal(0.0, 0.5));
        if (multi_level_) {
          fresh.level = estimate_.level;
          if (baro_ready_) fresh.baro_offset = altimeter_.altitude() - elevation(fresh.level);
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
      if (venue_->inside(jittered) && !venue_->crosses_wall(p.pos, jittered, p.level)) p.pos = jittered;
    }
    p.weight = step;
  }
  particles_.swap(scratch_);
}

void ParticleFilter::update_estimate(double t) {
  if (multi_level_) {
    // The reported level is the weighted mode; position statistics use only
    // the particles on that level so a split cloud does not average floors.
    std::fill(level_weight_.begin(), level_weight_.end(), 0.0);
    for (const auto& p : particles_) level_weight_[static_cast<std::size_t>(p.level)] += p.weight;
    estimate_.level =
        static_cast<int>(std::max_element(level_weight_.begin(), level_weight_.end()) - level_weight_.begin());
    estimate_.z = elevation(estimate_.level);
  }
  const int level = estimate_.level;
  const double level_w = multi_level_ ? level_weight_[static_cast<std::size_t>(level)] : 1.0;
  const auto counts = [&](const Particle& p) { return !multi_level_ || p.level == level; };

  double sx = 0.0, sy = 0.0, ss = 0.0, sc = 0.0, so = 0.0;
  for (const auto& p : particles_) {
    if (!counts(p)) continue;
    sx += p.weight * p.pos.x;
    sy += p.weight * p.pos.y;
    ss += p.weight * std::sin(p.heading);
    sc += p.weight * std::cos(p.heading);
    so += p.weight * p.rssi_offset;
  }
  if (multi_level_ && level_w > 0.0) {
    sx /= level_w;
    sy /= level_w;
    so /= level_w;
  }
  mean_rssi_offset_ = so;
  double cxx = 0.0, cxy = 0.0, cyy = 0.0;
  for (const auto& p : particles_) {
    if (!counts(p)) continue;
    const double dx = p.pos.x - sx;
    const double dy = p.pos.y - sy;
    cxx += p.weight * dx * dx;
    cxy += p.weight * dx * dy;
    cyy += p.weight * dy * dy;
  }
  if (multi_level_ && level_w > 0.0) {
    cxx /= level_w;
    cxy /= level_w;
    cyy /= level_w;
  }
  estimate_.t = t;
  estimate_.pos = {sx, sy};
  estimate_.heading = std::atan2(ss, sc);
  estimate_.cov_xx = cxx;
  estimate_.cov_xy = cxy;
  estimate_.cov_yy = cyy;
  estimate_.valid = true;
}

}  // namespace ipe
