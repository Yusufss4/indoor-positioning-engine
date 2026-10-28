#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "ipe/rng.hpp"

using ipe::Rng;

TEST(Rng, SameSeedSameStream) {
  Rng a(42), b(42);
  for (int i = 0; i < 1000; ++i) ASSERT_EQ(a.next_u64(), b.next_u64());
}

TEST(Rng, DifferentSeedDifferentStream) {
  Rng a(1), b(2);
  int equal = 0;
  for (int i = 0; i < 100; ++i) equal += a.next_u64() == b.next_u64() ? 1 : 0;
  EXPECT_EQ(equal, 0);
}

// Golden values pin the generator so results are identical across standard
// libraries and in the WebAssembly build.
TEST(Rng, GoldenValues) {
  Rng r(42);
  const std::vector<std::uint64_t> expected = {1546998764402558742ULL, 6990951692964543102ULL, 12544586762248559009ULL,
                                               17057574109182124193ULL};
  for (auto v : expected) EXPECT_EQ(r.next_u64(), v);
}

TEST(Rng, UniformRangeAndMoments) {
  Rng r(7);
  double sum = 0.0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double u = r.uniform();
    ASSERT_GE(u, 0.0);
    ASSERT_LT(u, 1.0);
    sum += u;
  }
  EXPECT_NEAR(sum / n, 0.5, 0.005);
}

TEST(Rng, NormalMoments) {
  Rng r(9);
  double sum = 0.0, sum_sq = 0.0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const double x = r.normal(3.0, 2.0);
    sum += x;
    sum_sq += x * x;
  }
  const double mean = sum / n;
  const double var = sum_sq / n - mean * mean;
  EXPECT_NEAR(mean, 3.0, 0.02);
  EXPECT_NEAR(std::sqrt(var), 2.0, 0.02);
}
