#include <gtest/gtest.h>

#include "ipe/georef.hpp"
#include "ipe/rng.hpp"

using namespace ipe;

TEST(GeoRef, RoundTripBelowOneCentimetreOverOneKilometre) {
  Rng rng(9);
  for (const double lat : {-60.0, -33.9, 0.0, 41.0, 51.5, 70.0}) {
    const GeoRef g{lat, rng.uniform(-180.0, 180.0), rng.uniform(-180.0, 180.0)};
    for (int i = 0; i < 200; ++i) {
      const Vec2 p{rng.uniform(-1000.0, 1000.0), rng.uniform(-1000.0, 1000.0)};
      const Vec2 back = g.to_local(g.to_wgs84(p));
      ASSERT_LT(distance(p, back), 0.01) << "lat " << lat;
    }
  }
}

TEST(GeoRef, OriginAndAxes) {
  const GeoRef g{51.5, -0.12, 0.0};
  const LatLon o = g.to_wgs84({0, 0});
  EXPECT_DOUBLE_EQ(o.lat, 51.5);
  EXPECT_DOUBLE_EQ(o.lon, -0.12);
  // One kilometre north is ~0.008993 degrees of latitude at this latitude
  // (meridian radius ~6.3713e6 m); east moves longitude only.
  const LatLon n = g.to_wgs84({0, 1000});
  EXPECT_NEAR(n.lat - 51.5, 0.008993, 2e-5);
  EXPECT_DOUBLE_EQ(n.lon, -0.12);
  const LatLon e = g.to_wgs84({1000, 0});
  EXPECT_DOUBLE_EQ(e.lat, 51.5);
  EXPECT_NEAR(e.lon + 0.12, 0.01444, 1e-4);
}

TEST(GeoRef, RotationTurnsLocalAxes) {
  // Local +x points north when the frame is rotated 90 degrees counter-clockwise from east.
  const GeoRef g{10.0, 20.0, 90.0};
  const LatLon p = g.to_wgs84({100, 0});
  EXPECT_GT(p.lat, 10.0);
  EXPECT_NEAR(p.lon, 20.0, 1e-12);
}
