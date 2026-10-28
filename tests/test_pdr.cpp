#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "ipe/geometry.hpp"
#include "ipe/pdr.hpp"
#include "ipe/rng.hpp"

using namespace ipe;

namespace {

/// Vertical acceleration pattern of `steps` steps at `freq` Hz whose
/// peak-to-peak amplitude matches the Weinberg model for `step_len`.
std::vector<ImuSample> walking_signal(int steps, double freq, double step_len, double k, double yaw_rate, double noise,
                                      std::uint64_t seed) {
  Rng rng(seed);
  const double rate = 50.0;
  const double amp = std::pow(step_len / k, 4.0);
  std::vector<ImuSample> out;
  const int n = static_cast<int>(steps / freq * rate);
  for (int i = 0; i < n; ++i) {
    ImuSample s;
    s.t = i / rate;
    const double phase = freq * s.t;
    s.accel = {rng.normal(0, noise), rng.normal(0, noise),
               9.80665 + 0.5 * amp * std::sin(2 * std::numbers::pi * phase) + rng.normal(0, noise)};
    s.gyro = {0, 0, yaw_rate};
    const double psi = yaw_rate * s.t;
    s.mag = {40 * std::sin(psi), 40 * std::cos(psi), -30};
    out.push_back(s);
  }
  return out;
}

}  // namespace

TEST(Pdr, CountsStepsAndEstimatesLength) {
  PdrProcessor pdr;
  const auto signal = walking_signal(60, 1.8, 0.7, 0.48, 0.0, 0.2, 1);
  int steps = 0;
  double total = 0.0;
  for (const auto& s : signal) {
    if (auto ev = pdr.process(s)) {
      ++steps;
      total += ev->length;
    }
  }
  EXPECT_NEAR(steps, 60, 2);
  EXPECT_NEAR(total / steps, 0.7, 0.05);
}

TEST(Pdr, NoStepsWhenStationary) {
  PdrProcessor pdr;
  Rng rng(5);
  int steps = 0;
  for (int i = 0; i < 1000; ++i) {
    ImuSample s;
    s.t = i / 50.0;
    s.accel = {rng.normal(0, 0.2), rng.normal(0, 0.2), 9.80665 + rng.normal(0, 0.2)};
    s.mag = {0, 40, -30};
    steps += pdr.process(s) ? 1 : 0;
  }
  EXPECT_EQ(steps, 0);
}

TEST(Pdr, HeadingDeltaIntegratesGyro) {
  PdrProcessor pdr;
  const double yaw_rate = 0.3;
  const auto signal = walking_signal(30, 1.8, 0.7, 0.48, yaw_rate, 0.1, 2);
  double sum_delta = 0.0;
  double first_t = -1.0, last_t = 0.0;
  for (const auto& s : signal) {
    if (auto ev = pdr.process(s)) {
      if (first_t < 0) {
        first_t = ev->t;
      } else {
        sum_delta += ev->heading_delta;
      }
      last_t = ev->t;
    }
  }
  EXPECT_NEAR(sum_delta, yaw_rate * (last_t - first_t), 0.02);
}

TEST(Pdr, MagHeadingConvention) {
  // Facing north (+y): psi = pi/2; facing east (+x): psi = 0.
  const auto facing = [](double psi) { return Vec3{40 * std::sin(psi), 40 * std::cos(psi), -30}; };
  for (double psi : {0.0, 0.5, std::numbers::pi / 2, -2.0, 3.0}) {
    EXPECT_NEAR(wrap_angle(PdrProcessor::heading_from_mag(facing(psi)) - psi), 0.0, 1e-9);
  }
}
