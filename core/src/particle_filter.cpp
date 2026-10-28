#include "ipe/particle_filter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ipe {

ParticleFilter::ParticleFilter(const Venue& venue, ParticleFilterConfig cfg, RssiModelConfig rssi_cfg)
    : venue_(&venue), cfg_(cfg), rssi_model_(venue, rssi_cfg), rng_(cfg.seed) {
  cfg_.num_particles = std::max(cfg_.num_particles, 1);
  health_.resize(venue.beacons().size());
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
  const double residual = rssi - (rssi_model_.expected_rssi(beacon, estimate_.pos) + mean_rssi_offset_);
  h.mean_residual = h.samples == 0 ? residual : h.mean_residual + cfg_.integrity_alpha * (residual - h.mean_residual);
  ++h.samples;

  if (!h.excluded && h.samples >= cfg_.integrity_min_samples &&
      std::abs(h.mean_residual) > cfg_.integrity_threshold_db) {
    // Never drop more than a third of the infrastructure on our own say-so.
    const auto excluded = std::count_if(health_.begin(), health_.end(), [](const BeaconHealth& b) { return b.excluded; });
    if (static_cast<std::size_t>(excluded + 1) * 3 <= health_.size()) h.excluded = true;
  } else if (h.excluded && std::abs(h.mean_residual) < 0.5 * cfg_.integrity_threshold_db) {
    h.excluded = false;
  }
  return !h.excluded;
}

Particle ParticleFilter::sample_uniform_particle() {
  const auto& b = venue_->bounds();
  Particle p;
  p.pos = {rng_.uniform(b.min_x, b.max_x), rng_.uniform(b.min_y, b.max_y)};
  p.heading = rng_.uniform(-std::numbers::pi, std::numbers::pi);
  p.step_scale = std::max(0.5, rng_.normal(1.0, cfg_.step_scale_sigma));
  p.rssi_offset = cfg_.estimate_rssi_offset ? rng_.normal(0.0, cfg_.rssi_offset_sigma) : 0.0;
  return p;
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
  const double heading_sigma = deg2rad(cfg_.heading_sigma_deg);
  for (auto& p : particles_) {
    if (p.weight <= 0.0) continue;
    p.heading = wrap_angle(p.heading + step.heading_delta + rng_.normal(0.0, heading_sigma));
    p.step_scale = std::clamp(p.step_scale + rng_.normal(0.0, cfg_.step_scale_walk), 0.5, 1.5);
    const double len = std::max(0.0, step.length * p.step_scale + rng_.normal(0.0, cfg_.step_length_sigma));
    const Vec2 next = p.pos + Vec2{std::cos(p.heading), std::sin(p.heading)} * len;

    if (!venue_->bounds().contains(next) || (cfg_.wall_constraint && venue_->crosses_wall(p.pos, next))) {
      p.weight = 0.0;
      continue;
    }
    p.pos = next;
  }
  normalize_or_recover();
  resample_if_needed();
  update_estimate(step.t);
}

void ParticleFilter::on_heading(double t, double heading, double sigma) {
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
  if (!check_beacon(beacon_index, rssi)) return;
  const Beacon& beacon = venue_->beacons()[beacon_index];

  loglik_.resize(particles_.size());
  double max_ll = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < particles_.size(); ++i) {
    auto& p = particles_[i];
    if (cfg_.estimate_rssi_offset) p.rssi_offset += rng_.normal(0.0, cfg_.rssi_offset_walk);
    if (p.weight <= 0.0) {
      loglik_[i] = -std::numeric_limits<double>::infinity();
      continue;
    }
    const double expected = rssi_model_.expected_rssi(beacon, p.pos) + p.rssi_offset;
    loglik_[i] = rssi_model_.log_likelihood(rssi - expected);
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
        if (venue_->bounds().contains(pos)) fresh.pos = pos;
        fresh.heading = wrap_angle(estimate_.heading + rng_.normal(0.0, 0.5));
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
      p = sample_uniform_particle();
    } else {
      const Vec2 jittered = p.pos + Vec2{rng_.normal(0.0, cfg_.roughening_m), rng_.normal(0.0, cfg_.roughening_m)};
      if (venue_->bounds().contains(jittered) && !venue_->crosses_wall(p.pos, jittered)) p.pos = jittered;
    }
    p.weight = step;
  }
  particles_.swap(scratch_);
}

void ParticleFilter::update_estimate(double t) {
  double sx = 0.0, sy = 0.0, ss = 0.0, sc = 0.0, so = 0.0;
  for (const auto& p : particles_) {
    sx += p.weight * p.pos.x;
    sy += p.weight * p.pos.y;
    ss += p.weight * std::sin(p.heading);
    sc += p.weight * std::cos(p.heading);
    so += p.weight * p.rssi_offset;
  }
  mean_rssi_offset_ = so;
  double cxx = 0.0, cxy = 0.0, cyy = 0.0;
  for (const auto& p : particles_) {
    const double dx = p.pos.x - sx;
    const double dy = p.pos.y - sy;
    cxx += p.weight * dx * dx;
    cxy += p.weight * dx * dy;
    cyy += p.weight * dy * dy;
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
