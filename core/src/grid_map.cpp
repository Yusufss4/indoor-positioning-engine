#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#include "ipe/grid_filter.hpp"
#include "ipe/reachable_area.hpp"

namespace ipe {

namespace {

std::uint8_t clamp_count(int n) { return static_cast<std::uint8_t>(std::clamp(n, 0, 255)); }

/// A wall, an endpoint or the beacon closer than this [m] to what it is measured against makes the wall's
/// shadow degenerate; its cells then get the exact test.
constexpr double kDegenerateM = 1e-2;
/// Cells closer than this [m] to a shadow edge get the exact test (scaled up for venues over 1 km).
constexpr double kEdgeBandM = 1e-5;

/// Half-plane n.p > c, with n a unit normal: the signed distance n.p - c is positive inside.
struct HalfPlane {
  double nx = 0.0;
  double ny = 0.0;
  double c = 0.0;
};

/// Inside of the line through p along the unit direction u: its left side, or its right side if left is false.
HalfPlane side_of(Vec2 p, Vec2 u, bool left) {
  const double s = left ? 1.0 : -1.0;
  const Vec2 n{-u.y * s, u.x * s};
  return {n.x, n.y, dot(n, p)};
}

/// The cells whose segment to beacon b crosses wall w (`segments_intersect` true, all orientations non-zero)
/// are the wall's shadow: the wedge from b between its rays through the two endpoints, beyond the wall's
/// line. None when the wall is too short, b too close to an endpoint, or b too close to the wall's line; the
/// caller then tests every cell.
std::optional<std::array<HalfPlane, 3>> shadow(Vec2 b, const Segment& w) {
  const Vec2 q1 = w.a, q2 = w.b;
  const double len = distance(q1, q2), d1 = distance(q1, b), d2 = distance(q2, b);
  if (len < kDegenerateM || d1 < kDegenerateM || d2 < kDegenerateM) return std::nullopt;
  const double area2 = cross(q1 - b, q2 - b);  // positive when q2 is left of b -> q1
  if (std::abs(area2) / len < kDegenerateM) return std::nullopt;
  return std::array<HalfPlane, 3>{
      side_of(b, (q1 - b) * (1.0 / d1), area2 > 0.0),                      // q2's side of the ray through q1
      side_of(b, (q2 - b) * (1.0 / d2), area2 < 0.0),                      // q1's side of the ray through q2
      side_of(q1, (q2 - q1) * (1.0 / len), cross(q2 - q1, b - q1) < 0.0),  // b's opposite side of the wall
  };
}

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
  // Equal to Venue::count_wall_crossings(centre(i), bp, level) for every cell, i.e. the number of the level's
  // walls w with segments_intersect(centre(i), bp, w.a, w.b) (the argument order of RssiModel::expected_rssi),
  // without testing every wall against every cell. Each wall's shadow (see shadow()) is the intersection of
  // three half-planes; along each lattice row it is an interval of columns:
  // - columns more than `band` inside all three are counted without a test, through a difference array;
  // - columns more than `band` outside one of them are left out;
  // - the columns in between, rarely more than one where a half-plane's edge crosses the row, get the
  //   original test.
  // That is exact. At a counted cell, each orientation the test computes is the length of a segment of at
  // least kDegenerateM (the wall, or the beacon to an endpoint) times a distance of at least `band`, so at
  // least 1e-7 in magnitude: far above the test's 1e-12 zero band and the rounding error of a cross product
  // at venue scale (about 1e-15 times the squared coordinates, 1e-9 at 1 km; the band grows with the square
  // beyond). The test then takes its generic branch, and the segments cross properly. A cell left out is
  // strictly on the wrong side of a line through the beacon, or on the beacon's side of the wall, by more
  // than `band`, where the segments cannot meet. Rounding in the conversion from x to columns is about 1e-13
  // of a column, against a band of 2e-5 of a 0.5 m column.
  const Vec2 bp = venue_->beacons()[beacon].pos;
  const std::size_t n = cells_.size();
  const auto nx = static_cast<std::size_t>(nx_);
  const std::size_t stride = nx + 1;
  const std::size_t per_level = nx * static_cast<std::size_t>(ny_);
  const double scale = std::max({1.0, std::abs(origin_.x), std::abs(origin_.y), std::abs(origin_.x + nx_ * cell_m_),
                                 std::abs(origin_.y + ny_ * cell_m_), std::abs(bp.x), std::abs(bp.y)}) /
                       1000.0;
  const double band = kEdgeBandM * std::max(1.0, scale * scale);
  constexpr double kInf = std::numeric_limits<double>::infinity();

  std::vector<std::int32_t> count(n, 0);
  std::vector<std::int32_t> diff;
  // Lattice column of a bound on the x of column centres, clamped to the lattice +- 2.
  const auto column = [&](double x) { return std::clamp((x - origin_.x) / cell_m_ - 0.5, -2.0, nx_ + 1.0); };
  const int levels = std::min(static_cast<int>(level_begin_.size()) - 1, venue_->level_count());
  for (int l = 0; l < levels; ++l) {
    const std::size_t begin = level_begin_[static_cast<std::size_t>(l)];
    const std::size_t end = level_begin_[static_cast<std::size_t>(l) + 1];
    const auto& walls = venue_->level(l).walls;
    if (begin == end || walls.empty()) continue;
    diff.assign(stride * static_cast<std::size_t>(ny_), 0);
    const std::int32_t* index = index_.data() + static_cast<std::size_t>(l) * per_level;
    for (const Segment& w : walls) {
      const auto test = [&](std::size_t i) {
        if (segments_intersect(centre(i), bp, w.a, w.b)) ++count[i];
      };
      const auto planes = shadow(bp, w);
      if (!planes) {
        for (std::size_t i = begin; i < end; ++i) test(i);
        continue;
      }
      for (int iy = 0; iy < ny_; ++iy) {
        const double y = origin_.y + (iy + 0.5) * cell_m_;
        // x intervals: `sure` more than band inside every half-plane, `maybe` less than band outside.
        double sure_lo = -kInf, sure_hi = kInf, maybe_lo = -kInf, maybe_hi = kInf;
        bool sure_any = true, maybe_any = true;
        for (const HalfPlane& h : *planes) {
          const double off = h.ny * y - h.c;  // signed distance = h.nx * x + off
          if (h.nx == 0.0) {
            if (off < -band) maybe_any = false;
            if (off <= band) sure_any = false;
            continue;
          }
          const double at_sure = (band - off) / h.nx, at_maybe = (-band - off) / h.nx;
          if (h.nx > 0.0) {
            sure_lo = std::max(sure_lo, at_sure);
            maybe_lo = std::max(maybe_lo, at_maybe);
          } else {
            sure_hi = std::min(sure_hi, at_sure);
            maybe_hi = std::min(maybe_hi, at_maybe);
          }
        }
        if (!maybe_any) continue;
        const int c_lo = std::max(0, static_cast<int>(std::ceil(column(maybe_lo))));
        const int c_hi = std::min(nx_ - 1, static_cast<int>(std::floor(column(maybe_hi))));
        if (c_lo > c_hi) continue;
        int s_lo = c_hi + 1, s_hi = c_hi;  // empty
        if (sure_any) {
          s_lo = std::max(c_lo, static_cast<int>(std::floor(column(sure_lo))) + 1);
          s_hi = std::min(c_hi, static_cast<int>(std::ceil(column(sure_hi))) - 1);
          if (s_lo > s_hi) s_lo = c_hi + 1, s_hi = c_hi;
        }
        const std::size_t row = static_cast<std::size_t>(iy) * stride;
        if (s_lo <= s_hi) {
          ++diff[row + static_cast<std::size_t>(s_lo)];
          --diff[row + static_cast<std::size_t>(s_hi) + 1];
        }
        const std::int32_t* cells_in_row = index + static_cast<std::size_t>(iy) * nx;
        for (int ix = c_lo; ix <= c_hi; ++ix) {
          if (ix == s_lo) ix = s_hi + 1;
          if (ix > c_hi) break;
          if (const std::int32_t i = cells_in_row[ix]; i >= 0) test(static_cast<std::size_t>(i));
        }
      }
    }
    for (std::size_t row = 0; row < diff.size(); row += stride) {
      std::int32_t run = 0;
      for (std::size_t ix = 0; ix < nx; ++ix) {
        run += diff[row + ix];
        diff[row + ix] = run;
      }
    }
    for (std::size_t i = begin; i < end; ++i) {
      count[i] += diff[static_cast<std::size_t>(cells_[i].iy) * stride + static_cast<std::size_t>(cells_[i].ix)];
    }
  }
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = clamp_count(count[i]);
  return out;
}

}  // namespace ipe
