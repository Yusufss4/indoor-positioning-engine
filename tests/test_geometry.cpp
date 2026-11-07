#include <gtest/gtest.h>

#include <numbers>

#include "ipe/geometry.hpp"

using namespace ipe;

TEST(Geometry, ProperIntersection) {
  EXPECT_TRUE(segments_intersect({0, 0}, {2, 2}, {0, 2}, {2, 0}));
  EXPECT_FALSE(segments_intersect({0, 0}, {1, 1}, {2, 0}, {3, 1}));
}

TEST(Geometry, TouchingEndpointCountsAsIntersection) {
  EXPECT_TRUE(segments_intersect({0, 0}, {1, 0}, {1, 0}, {1, 1}));
  EXPECT_TRUE(segments_intersect({0, 0}, {2, 0}, {1, 0}, {1, 1}));
}

TEST(Geometry, CollinearOverlapAndDisjoint) {
  EXPECT_TRUE(segments_intersect({0, 0}, {2, 0}, {1, 0}, {3, 0}));
  EXPECT_FALSE(segments_intersect({0, 0}, {1, 0}, {2, 0}, {3, 0}));
}

TEST(Geometry, ParallelNonCollinear) { EXPECT_FALSE(segments_intersect({0, 0}, {2, 0}, {0, 1}, {2, 1})); }

TEST(Geometry, IntersectionParameter) {
  EXPECT_NEAR(segment_intersection_param({0, 0}, {4, 0}, {1, -1}, {1, 1}), 0.25, 1e-12);
  EXPECT_NEAR(segment_intersection_param({0, 0}, {4, 0}, {4, -1}, {4, 1}), 1.0, 1e-12);
  EXPECT_LT(segment_intersection_param({0, 0}, {4, 0}, {5, -1}, {5, 1}), 0.0);
  EXPECT_LT(segment_intersection_param({0, 0}, {2, 0}, {0, 1}, {2, 1}), 0.0);
  // Collinear overlap: the first shared point.
  EXPECT_NEAR(segment_intersection_param({0, 0}, {4, 0}, {3, 0}, {1, 0}), 0.25, 1e-12);
  // Agrees with segments_intersect on random pairs.
  for (int i = 0; i < 2000; ++i) {
    auto c = [&](int k) { return static_cast<double>((i * 7919 + k * 104729) % 97) / 9.7; };
    const Vec2 p1{c(1), c(2)}, p2{c(3), c(4)}, q1{c(5), c(6)}, q2{c(7), c(8)};
    ASSERT_EQ(segment_intersection_param(p1, p2, q1, q2) >= 0.0, segments_intersect(p1, p2, q1, q2)) << i;
  }
}

TEST(Geometry, WrapAngle) {
  EXPECT_NEAR(wrap_angle(0.0), 0.0, 1e-12);
  EXPECT_NEAR(wrap_angle(3 * std::numbers::pi), std::numbers::pi, 1e-9);
  EXPECT_NEAR(wrap_angle(-3 * std::numbers::pi / 2), std::numbers::pi / 2, 1e-12);
  for (double a = -20.0; a < 20.0; a += 0.37) {
    const double w = wrap_angle(a);
    EXPECT_GT(w, -std::numbers::pi - 1e-12);
    EXPECT_LE(w, std::numbers::pi + 1e-12);
    EXPECT_NEAR(std::sin(w), std::sin(a), 1e-9);
    EXPECT_NEAR(std::cos(w), std::cos(a), 1e-9);
  }
}

TEST(Geometry, BoundsContains) {
  const Bounds b{0, 0, 10, 5};
  EXPECT_TRUE(b.contains({5, 2}));
  EXPECT_TRUE(b.contains({0, 0}));
  EXPECT_FALSE(b.contains({10.1, 2}));
  EXPECT_DOUBLE_EQ(b.width(), 10.0);
  EXPECT_DOUBLE_EQ(b.height(), 5.0);
}
