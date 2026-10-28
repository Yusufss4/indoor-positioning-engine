#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "ipe/baro.hpp"

using namespace ipe;

TEST(Baro, BarometricFormulaRoundTrips) {
  for (const double h : {-50.0, 0.0, 4.0, 120.0, 1500.0}) {
    EXPECT_NEAR(pressure_to_altitude(altitude_to_pressure(h)), h, 1e-6);
  }
  EXPECT_NEAR(pressure_to_altitude(1013.25), 0.0, 1e-9);
}

TEST(Baro, OneFloorIsAboutHalfAHectopascal) {
  // Near sea level the gradient is ~ -0.12 hPa per metre (8.3 m per hPa).
  const double dp = altitude_to_pressure(0.0) - altitude_to_pressure(4.0);
  EXPECT_NEAR(dp, 0.48, 0.01);
}

TEST(Baro, AltimeterReportsRelativeAltitude) {
  BaroAltimeter alt({0.5, 1013.25});
  const double p0 = 1002.0;  // weather and device offset make absolute values meaningless
  EXPECT_DOUBLE_EQ(alt.process(0.0, p0), 0.0);
  double h = 0.0;
  for (int i = 1; i <= 100; ++i) h = alt.process(0.1 * i, altitude_to_pressure(pressure_to_altitude(p0) + 4.0));
  EXPECT_NEAR(h, 4.0, 0.01);
}

TEST(Baro, LowPassSuppressesShortSpikes) {
  BaroAltimeter alt({1.0, 1013.25});
  alt.process(0.0, 1000.0);
  double peak = 0.0;
  for (int i = 1; i <= 50; ++i) {
    const double spike = (i == 10) ? 0.3 : 0.0;  // single-sample door slam, ~2.5 m equivalent
    peak = std::max(peak, std::abs(alt.process(0.1 * i, 1000.0 + spike)));
  }
  EXPECT_LT(peak, 0.4);
}
