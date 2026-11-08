// Beacon-driven proposals of the particle filter (`pf.init_proposal`,
// `pf.recovery_proposal`): range rings around a heard beacon, and the
// importance weights that keep the seeded cloud an exact posterior.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ipe/particle_filter.hpp"

namespace ipe {

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
  const double spread_db = rssi_model_.config().sigma_db;
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
  double total_area = 0.0;
  for (int l = 0; l < venue_->level_count(); ++l) total_area += venue_->walkable_area(l);
  const double z = attempts > 0 ? static_cast<double>(accepted) / static_cast<double>(attempts) : 1.0;
  const double miss_all = std::pow(1.0 - z, kRingTries);
  const double ring_scale = z > 0.0 ? (1.0 - miss_all) / z : static_cast<double>(kRingTries);
  double sum = 0.0;
  for (auto& p : particles_) {
    const Ring ring = ring_for(beacon, rssi, p.rssi_offset);
    const double ring_pos =
        area * ring_scale * ring_density(distance(p.pos, beacon.pos), ring.log10_mean, ring.log10_sigma) + miss_all;
    if (venue_->has_level_footprints()) {
      // The prior is uniform over the walkable area of all levels together
      // (levels are drawn by area), and zero off a level's own footprint.
      p.weight = venue_->inside(p.pos, p.level)
                     ? 1.0 / ((1.0 - f) + f * total_area * ring_level_prob(beacon.level, p.level) * ring_pos / area)
                     : 0.0;
    } else {
      p.weight = 1.0 / ((1.0 - f) + f * levels * ring_level_prob(beacon.level, p.level) * ring_pos);
    }
    sum += p.weight;
  }
  for (auto& p : particles_) p.weight /= sum;
  radio_seeded_ = true;
  localized_ = true;
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

double ParticleFilter::apparent_range(std::size_t beacon_index, double rssi) const {
  const Beacon& b = venue_->beacons()[beacon_index];
  return (b.tx_power_dbm - rssi) / b.path_loss_exp;
}

void ParticleFilter::flush_pending() {
  for (std::size_t i = 0; i < pending_count_; ++i) update_rssi(pending_[i].t, pending_[i].beacon, pending_[i].rssi);
  pending_count_ = 0;
}

}  // namespace ipe
