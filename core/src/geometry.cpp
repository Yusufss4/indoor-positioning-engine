#include "ipe/geometry.hpp"

#include <algorithm>

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

}  // namespace ipe
