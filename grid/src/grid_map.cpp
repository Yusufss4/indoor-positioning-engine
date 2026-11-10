#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include "ipe/grid_filter.hpp"
#include "ipe/reachable_area.hpp"

namespace ipe {

namespace {

std::uint8_t clamp_count(int n) { return static_cast<std::uint8_t>(std::clamp(n, 0, 255)); }

}  // namespace

GridMap::GridMap(const Venue& venue, const GridFilterConfig& cfg) : venue_(&venue) {
  std::optional<ReachableArea> reach;
  if (cfg.closed_region_fraction > 0.0) {
    reach.emplace(venue, cfg.closed_region_fraction, cfg.closed_region_min_m2);
    if (!reach->any_closed()) reach.reset();
  }
  const int levels = std::max(venue.level_count(), 1);
  double area = 0.0;
  for (int l = 0; l < levels; ++l) area += venue.walkable_area(l) - (reach ? reach->closed_area(l) : 0.0);
  cell_m_ = std::max(cfg.cell_m, std::sqrt(std::max(area, 0.0) / std::max(cfg.max_cells, 1.0)));

  const Bounds& b = venue.bounds();
  origin_ = {b.min_x, b.min_y};
  nx_ = std::max(1, static_cast<int>(std::ceil(b.width() / cell_m_)));
  ny_ = std::max(1, static_cast<int>(std::ceil(b.height() / cell_m_)));
  radius_ = static_cast<int>(std::ceil(cfg.max_reach_m / cell_m_));
  if (radius_ > kMaxRadius) radius_ = kMaxRadius;
  if (radius_ < 1) radius_ = 1;

  const std::size_t per_level = static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_);
  index_.assign(per_level * static_cast<std::size_t>(levels), -1);
  level_begin_.assign(static_cast<std::size_t>(levels) + 1, 0);
  for (int l = 0; l < levels; ++l) {
    level_begin_[static_cast<std::size_t>(l)] = cells_.size();
    for (int iy = 0; iy < ny_; ++iy) {
      for (int ix = 0; ix < nx_; ++ix) {
        const Vec2 c = lattice_centre(ix, iy);
        if (!venue.inside(c, l) || (reach && !reach->open(c, l))) continue;
        index_[static_cast<std::size_t>(l) * per_level + static_cast<std::size_t>(iy) * static_cast<std::size_t>(nx_) +
               static_cast<std::size_t>(ix)] = static_cast<std::int32_t>(cells_.size());
        Cell cell;
        cell.ix = ix;
        cell.iy = iy;
        cell.level = l;
        cells_.push_back(cell);
      }
    }
  }
  level_begin_[static_cast<std::size_t>(levels)] = cells_.size();
  if (cells_.empty()) throw std::invalid_argument("GridMap: the venue has no walkable cell");

  // Transition masks. A move is symmetric, so each pair of cells is tested once.
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    Cell& c = cells_[i];
    const int self = bit(0, 0);
    c.mask[static_cast<std::size_t>(self >> 6)] |= std::uint64_t{1} << (self & 63);
    const Vec2 from = centre(i);
    for (int dy = 0; dy <= radius_; ++dy) {
      for (int dx = -radius_; dx <= radius_; ++dx) {
        if (dy == 0 && dx <= 0) continue;
        const std::int32_t j = at(c.level, c.ix + dx, c.iy + dy);
        if (j < 0) continue;
        if (venue.crosses_wall(from, centre(static_cast<std::size_t>(j)), c.level)) continue;
        const int fwd = bit(dx, dy), back = bit(-dx, -dy);
        c.mask[static_cast<std::size_t>(fwd >> 6)] |= std::uint64_t{1} << (fwd & 63);
        Cell& o = cells_[static_cast<std::size_t>(j)];
        o.mask[static_cast<std::size_t>(back >> 6)] |= std::uint64_t{1} << (back & 63);
      }
    }
  }

  // Rooms and connector links.
  const auto& rooms = venue.rooms();
  room_begin_.assign(cells_.size() + 1, 0);
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    Cell& c = cells_[i];
    const Vec2 p = centre(i);
    room_begin_[i] = static_cast<std::uint32_t>(room_list_.size());
    if (!rooms.empty()) {
      for (std::size_t r : venue.room_candidates(p, c.level)) {
        if (rooms[r].contains(p)) room_list_.push_back(static_cast<std::uint32_t>(r));
      }
    }
    if (const Connector* conn = venue.connector_at(p, c.level)) {
      const auto it = std::find(conn->levels.begin(), conn->levels.end(), c.level);
      if (it + 1 != conn->levels.end()) c.up = at(*(it + 1), c.ix, c.iy);
      if (it != conn->levels.begin()) c.down = at(*(it - 1), c.ix, c.iy);
      if (c.up >= 0 || c.down >= 0) connector_cells_.push_back(static_cast<std::uint32_t>(i));
    }
  }
  room_begin_[cells_.size()] = static_cast<std::uint32_t>(room_list_.size());

  const std::size_t beacons = venue.beacons().size();
  walls_.resize(beacons);
  walls_once_ = std::make_unique<std::once_flag[]>(std::max<std::size_t>(beacons, 1));
}

std::span<const std::uint8_t> GridMap::wall_counts(std::size_t beacon) const {
  std::call_once(walls_once_[beacon], [&] { walls_[beacon] = exact_wall_counts(beacon); });
  return walls_[beacon];
}

std::vector<std::uint8_t> GridMap::exact_wall_counts(std::size_t beacon) const {
  // Same argument order as RssiModel::expected_rssi, so touching cases count the same way.
  const Vec2 bp = venue_->beacons()[beacon].pos;
  std::vector<std::uint8_t> out(cells_.size());
  for (std::size_t i = 0; i < cells_.size(); ++i) {
    out[i] = clamp_count(venue_->count_wall_crossings(centre(i), bp, cells_[i].level));
  }
  return out;
}

}  // namespace ipe
