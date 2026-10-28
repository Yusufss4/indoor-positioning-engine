#pragma once

#include <cmath>
#include <numbers>
#include <vector>

namespace ipe {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;

  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
  constexpr Vec2& operator+=(Vec2 o) {
    x += o.x;
    y += o.y;
    return *this;
  }
  [[nodiscard]] double norm() const { return std::hypot(x, y); }
  [[nodiscard]] constexpr double norm2() const { return x * x + y * y; }
};

constexpr double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double distance(Vec2 a, Vec2 b) { return (a - b).norm(); }

struct Segment {
  Vec2 a;
  Vec2 b;
};

struct Bounds {
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;

  [[nodiscard]] constexpr bool contains(Vec2 p) const {
    return p.x >= min_x && p.x <= max_x && p.y >= min_y && p.y <= max_y;
  }
  [[nodiscard]] constexpr double width() const { return max_x - min_x; }
  [[nodiscard]] constexpr double height() const { return max_y - min_y; }
};

/// Wraps an angle to (-pi, pi].
inline double wrap_angle(double a) {
  constexpr double two_pi = 2.0 * std::numbers::pi;
  a = std::fmod(a + std::numbers::pi, two_pi);
  if (a <= 0.0) a += two_pi;
  return a - std::numbers::pi;
}

inline constexpr double deg2rad(double d) { return d * std::numbers::pi / 180.0; }
inline constexpr double rad2deg(double r) { return r * 180.0 / std::numbers::pi; }

/// Proper or touching intersection test between segments p1-p2 and q1-q2.
/// Collinear overlaps count as intersections so that a path sliding along a
/// wall is treated as blocked.
bool segments_intersect(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2);

inline bool segments_intersect(const Segment& s, const Segment& t) { return segments_intersect(s.a, s.b, t.a, t.b); }

/// Simple polygon as a vertex ring; the closing edge back to the first vertex is implicit.
using Polygon = std::vector<Vec2>;

/// Even-odd point-in-polygon test. Points exactly on an edge may go either way.
bool point_in_polygon(Vec2 p, const Polygon& poly);

}  // namespace ipe
