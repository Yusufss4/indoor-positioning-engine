#include "ipe/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {
namespace {

int orientation(Vec2 a, Vec2 b, Vec2 c) {
  constexpr double eps = 1e-12;
  const double v = cross(b - a, c - a);
  if (v > eps) return 1;
  if (v < -eps) return -1;
  return 0;
}

bool on_segment(Vec2 a, Vec2 b, Vec2 p) {
  return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y &&
         p.y <= std::max(a.y, b.y);
}

}  // namespace

bool segments_intersect(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) {
  const int o1 = orientation(p1, p2, q1);
  const int o2 = orientation(p1, p2, q2);
  const int o3 = orientation(q1, q2, p1);
  const int o4 = orientation(q1, q2, p2);

  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(p1, p2, q1)) return true;
  if (o2 == 0 && on_segment(p1, p2, q2)) return true;
  if (o3 == 0 && on_segment(q1, q2, p1)) return true;
  if (o4 == 0 && on_segment(q1, q2, p2)) return true;
  return false;
}

double segment_intersection_param(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) {
  constexpr double eps = 1e-12;
  const Vec2 r = p2 - p1;
  const Vec2 s = q2 - q1;
  const Vec2 qp = q1 - p1;
  const double denom = cross(r, s);
  if (std::abs(denom) > eps) {
    const double t = cross(qp, s) / denom;
    const double u = cross(qp, r) / denom;
    constexpr double tol = 1e-9;
    if (t < -tol || t > 1.0 + tol || u < -tol || u > 1.0 + tol) return -1.0;
    return std::clamp(t, 0.0, 1.0);
  }
  if (std::abs(cross(qp, r)) > eps) return -1.0;  // parallel, not collinear
  const double r2 = r.norm2();
  if (r2 <= 0.0) return on_segment(q1, q2, p1) ? 0.0 : -1.0;
  const double t0 = dot(qp, r) / r2;
  const double t1 = dot(q2 - p1, r) / r2;
  const double lo = std::max(0.0, std::min(t0, t1));
  const double hi = std::min(1.0, std::max(t0, t1));
  return lo <= hi ? lo : -1.0;
}

bool point_in_polygon(Vec2 p, const Polygon& poly) {
  bool inside = false;
  const std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 a = poly[i];
    const Vec2 b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
  }
  return inside;
}

double polygon_area(const Polygon& poly) {
  double twice = 0.0;
  const std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) twice += cross(poly[j], poly[i]);
  return 0.5 * std::abs(twice);
}

}  // namespace ipe
