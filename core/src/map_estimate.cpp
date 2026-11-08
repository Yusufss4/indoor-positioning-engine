#include "ipe/map_estimate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ipe {

MapConsistentEstimate::MapConsistentEstimate(const Venue& venue, std::size_t particle_capacity) : venue_(&venue) {
  const auto& b = venue.bounds();
  // Coarser cells for huge venues keep the histogram under a million cells.
  cell_m_ = std::max(kCellM, std::sqrt(b.width() * b.height() / 1e6));
  nx_ = std::max(1, static_cast<int>(std::ceil(b.width() / cell_m_)));
  ny_ = std::max(1, static_cast<int>(std::ceil(b.height() / cell_m_)));
  weight_.assign(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_), 0.0);
  touched_.reserve(particle_capacity);
}

std::size_t MapConsistentEstimate::cell(Vec2 p) const {
  const auto& b = venue_->bounds();
  const int x = std::clamp(static_cast<int>(std::floor((p.x - b.min_x) / cell_m_)), 0, nx_ - 1);
  const int y = std::clamp(static_cast<int>(std::floor((p.y - b.min_y) / cell_m_)), 0, ny_ - 1);
  return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x);
}

Vec2 MapConsistentEstimate::position(std::span<const Particle> particles, Vec2 mean, int level, bool multi_level) {
  const auto counts = [&](const Particle& p) { return p.weight > 0.0 && (!multi_level || p.level == level); };

  // The mean is fine if it is walkable and in sight of the particle closest to it.
  const Particle* nearest = nullptr;
  double best_d2 = std::numeric_limits<double>::infinity();
  for (const auto& p : particles) {
    if (!counts(p)) continue;
    const double d2 = (p.pos - mean).norm2();
    if (d2 < best_d2) {
      best_d2 = d2;
      nearest = &p;
    }
  }
  if (!nearest) return mean;
  if (venue_->inside(mean, multi_level ? level : -1) && !venue_->crosses_wall(nearest->pos, mean, level)) return mean;

  // Dominant mode: the heaviest 3x3 block of a coarse weight histogram.
  for (const auto& p : particles) {
    if (!counts(p)) continue;
    const std::size_t c = cell(p.pos);
    if (weight_[c] == 0.0) touched_.push_back(c);
    weight_[c] += p.weight;
  }
  const auto nx = static_cast<std::size_t>(nx_);
  const auto ny = static_cast<std::size_t>(ny_);
  std::size_t best_cell = touched_.front();
  double best_w = -1.0;
  for (std::size_t c : touched_) {
    const std::size_t cx = c % nx, cy = c / nx;
    double w = 0.0;
    for (std::size_t y = cy > 0 ? cy - 1 : 0; y <= std::min(cy + 1, ny - 1); ++y) {
      for (std::size_t x = cx > 0 ? cx - 1 : 0; x <= std::min(cx + 1, nx - 1); ++x) w += weight_[y * nx + x];
    }
    if (w > best_w) {
      best_w = w;
      best_cell = c;
    }
  }
  for (std::size_t c : touched_) weight_[c] = 0.0;
  touched_.clear();

  const auto bx = static_cast<long>(best_cell % nx), by = static_cast<long>(best_cell / nx);
  const auto in_block = [&](const Particle& p) {
    const std::size_t c = cell(p.pos);
    return std::abs(static_cast<long>(c % nx) - bx) <= 1 && std::abs(static_cast<long>(c / nx) - by) <= 1;
  };
  Vec2 centre{};
  double w_block = 0.0;
  for (const auto& p : particles) {
    if (!counts(p) || !in_block(p)) continue;
    centre += p.pos * p.weight;
    w_block += p.weight;
  }
  centre = centre * (1.0 / w_block);
  const Particle* anchor = nullptr;
  best_d2 = std::numeric_limits<double>::infinity();
  for (const auto& p : particles) {
    if (!counts(p) || !in_block(p)) continue;
    const double d2 = (p.pos - centre).norm2();
    if (d2 < best_d2) {
      best_d2 = d2;
      anchor = &p;
    }
  }
  // The block can still straddle a wall: keep the particles on the anchor's side.
  Vec2 mode{};
  double w_mode = 0.0;
  for (const auto& p : particles) {
    if (!counts(p) || !in_block(p) || venue_->crosses_wall(anchor->pos, p.pos, level)) continue;
    mode += p.pos * p.weight;
    w_mode += p.weight;
  }
  if (w_mode <= 0.0) return anchor->pos;
  mode = mode * (1.0 / w_mode);
  if (venue_->inside(mode, multi_level ? level : -1) && !venue_->crosses_wall(anchor->pos, mode, level)) return mode;
  return anchor->pos;
}

}  // namespace ipe
