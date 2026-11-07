#include <gtest/gtest.h>

#include "ipe/beacon_integrity.hpp"

using namespace ipe;

namespace {

BeaconIntegrityMonitor::Settings settings() { return {10.0, 0.1, 8, 3.0}; }

}  // namespace

TEST(BeaconIntegrity, ExcludesAPersistentlyBiasedBeacon) {
  BeaconIntegrityMonitor m(6, settings());
  for (int k = 0; k < 7; ++k) EXPECT_TRUE(m.admit(0, 15.0)) << "not before min_samples, k=" << k;
  EXPECT_FALSE(m.admit(0, 15.0));
  EXPECT_TRUE(m.excluded(0));
  EXPECT_FALSE(m.excluded(1));
  EXPECT_TRUE(m.admit(1, 2.0));
}

TEST(BeaconIntegrity, ReadmitsBelowHalfTheThreshold) {
  BeaconIntegrityMonitor m(6, settings());
  for (int k = 0; k < 8; ++k) m.admit(0, 15.0);
  ASSERT_TRUE(m.excluded(0));
  // The mean residual decays; above 5 dB the beacon stays out (hysteresis).
  int calls = 0;
  while (m.excluded(0)) {
    m.admit(0, 0.0);
    ++calls;
  }
  // 15 dB * 0.9^n < 5 dB first holds at n = 11.
  EXPECT_EQ(calls, 11);
}

TEST(BeaconIntegrity, NeverExcludesMoreThanAThird) {
  BeaconIntegrityMonitor m(6, settings());
  for (std::size_t b = 0; b < 6; ++b) {
    for (int k = 0; k < 8; ++k) m.admit(b, 20.0);
  }
  int excluded = 0;
  for (std::size_t b = 0; b < 6; ++b) excluded += m.excluded(b) ? 1 : 0;
  EXPECT_EQ(excluded, 2);
}

TEST(BeaconIntegrity, JudgesOnlyTightEstimates) {
  const BeaconIntegrityMonitor m(3, settings());
  EXPECT_TRUE(m.can_judge(2.9));
  EXPECT_TRUE(m.can_judge(3.0));
  EXPECT_FALSE(m.can_judge(3.1));
  EXPECT_FALSE(m.excluded(99));  // out of range is never excluded
}
