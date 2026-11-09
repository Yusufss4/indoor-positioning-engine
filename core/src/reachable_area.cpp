#include "ipe/reachable_area.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {

namespace {

int find_root(std::vector<int>& parent, int i) {
  while (parent[static_cast<std::size_t>(i)] != i) {
    const auto k = static_cast<std::size_t>(i);
    parent[k] = parent[static_cast<std::size_t>(parent[k])];
    i = parent[k];
  }
  return i;
}

}  // namespace

ReachableArea::ReachableArea(const Venue& venue, double max_fraction, double min_m2) : venue_(&venue) {
  const auto& b = venue.bounds();
  cell_m_ = std::max(kCellM, std::sqrt(b.width() * b.height() / kMaxCells));
  nx_ = std::max(1, static_cast<int>(std::ceil(b.width() / cell_m_)));
  ny_ = std::max(1, static_cast<int>(std::ceil(b.height() / cell_m_)));
  levels_.resize(static_cast<std::size_t>(std::max(venue.level_count(), 1)));
  for (int l = 0; l < venue.level_count(); ++l) build_level(l, max_fraction, min_m2);
}

Vec2 ReachableArea::centre(int x, int y) const {
  const auto& b = venue_->bounds();
  return {b.min_x + (x + 0.5) * cell_m_, b.min_y + (y + 0.5) * cell_m_};
}

void ReachableArea::build_level(int level, double max_fraction, double min_m2) {
  const auto n = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
  // Union-find over the cells on the map (parent -1: off the map); two neighbours are joined when the
  // segment between their centres crosses no wall.
  std::vector<int> parent(n, -1);
  for (int y = 0; y < ny_; ++y) {
    for (int x = 0; x < nx_; ++x) {
      if (venue_->inside(centre(x, y), level)) parent[static_cast<std::size_t>(y * nx_ + x)] = y * nx_ + x;
    }
  }
  const auto join = [&](int x0, int y0, int x1, int y1) {
    const int i = y0 * nx_ + x0, j = y1 * nx_ + x1;
    if (parent[static_cast<std::size_t>(i)] < 0 || parent[static_cast<std::size_t>(j)] < 0) return;
    if (venue_->crosses_wall(centre(x0, y0), centre(x1, y1), level)) return;
    parent[static_cast<std::size_t>(find_root(parent, i))] = find_root(parent, j);
  };
  for (int y = 0; y < ny_; ++y) {
    for (int x = 0; x < nx_; ++x) {
      if (x + 1 < nx_) join(x, y, x + 1, y);
      if (y + 1 < ny_) join(x, y, x, y + 1);
    }
  }

  // Cells per region, and whether a stairwell or lift lies in it (walkers arrive through those).
  std::vector<int> cells(n, 0);
  std::vector<char> connector(n, 0);
  int largest = 0;
  const bool has_connectors = !venue_->connectors().empty();
  for (std::size_t i = 0; i < n; ++i) {
    if (parent[i] < 0) continue;
    const auto r = static_cast<std::size_t>(find_root(parent, static_cast<int>(i)));
    largest = std::max(largest, ++cells[r]);
    if (has_connectors && !connector[r]) {
      const int x = static_cast<int>(i) % nx_, y = static_cast<int>(i) / nx_;
      if (venue_->connector_at(centre(x, y), level)) connector[r] = 1;
    }
  }

  const double cell_area = cell_m_ * cell_m_;
  LevelGrid& grid = levels_[static_cast<std::size_t>(level)];
  grid.state.assign(n, kOff);
  for (std::size_t i = 0; i < n; ++i) {
    if (parent[i] < 0) continue;
    const auto r = static_cast<std::size_t>(find_root(parent, static_cast<int>(i)));
    const double area = cells[r] * cell_area;
    const bool closed = !connector[r] && area >= min_m2 && area < max_fraction * largest * cell_area;
    grid.state[i] = closed ? kClosed : kOpen;
    if (closed) grid.closed_m2 += cell_area;
  }
  any_closed_ = any_closed_ || grid.closed_m2 > 0.0;
}

bool ReachableArea::open(Vec2 p, int level) const {
  const LevelGrid& grid = levels_[static_cast<std::size_t>(level)];
  if (grid.closed_m2 <= 0.0) return true;
  const auto& b = venue_->bounds();
  const int x0 = static_cast<int>(std::floor((p.x - b.min_x) / cell_m_));
  const int y0 = static_cast<int>(std::floor((p.y - b.min_y) / cell_m_));
  if (x0 < 0 || y0 < 0 || x0 >= nx_ || y0 >= ny_) return true;
  const auto state = [&](int x, int y) { return grid.state[static_cast<std::size_t>(y * nx_ + x)]; };

  // Away from walls the cells around p all belong to one region, and so does p.
  const std::uint8_t here = state(x0, y0);
  bool uniform = true;
  for (int y = y0 - 1; y <= y0 + 1 && uniform; ++y) {
    for (int x = x0 - 1; x <= x0 + 1 && uniform; ++x) {
      uniform = x >= 0 && y >= 0 && x < nx_ && y < ny_ && state(x, y) == here;
    }
  }
  if (uniform) return here != kClosed;

  // Next to a wall: p is in the region of the nearest cell centre it can see.
  for (int ring = 0; ring <= 2; ++ring) {
    for (int y = y0 - ring; y <= y0 + ring; ++y) {
      for (int x = x0 - ring; x <= x0 + ring; ++x) {
        if (std::max(std::abs(x - x0), std::abs(y - y0)) != ring) continue;
        if (x < 0 || y < 0 || x >= nx_ || y >= ny_) continue;
        const std::uint8_t s = state(x, y);
        if (s == kOff || venue_->crosses_wall(centre(x, y), p, level)) continue;
        return s != kClosed;
      }
    }
  }
  return true;
}

}  // namespace ipe
