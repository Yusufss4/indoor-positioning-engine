#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>

namespace ipe {

/// Deterministic random number generator (xoshiro256** seeded by splitmix64).
///
/// std::normal_distribution and friends are implementation-defined, so the
/// same seed gives different streams on libstdc++, libc++ and MSVC. The
/// regression suite and the WebAssembly demo rely on bit-identical streams,
/// so all sampling goes through this class instead.
class Rng {
 public:
  explicit Rng(std::uint64_t seed = 0x5eed) { reseed(seed); }

  void reseed(std::uint64_t seed) {
    for (auto& word : s_) word = splitmix64(seed);
    has_spare_ = false;
  }

  std::uint64_t next_u64() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  /// Uniform in [0, 1).
  double uniform() { return static_cast<double>(next_u64() >> 11) * 0x1.0p-53; }

  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

  /// Standard normal via the Box-Muller transform.
  double normal() {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    double u1 = uniform();
    while (u1 <= 0.0) u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = 2.0 * std::numbers::pi * u2;
    spare_ = r * std::sin(theta);
    has_spare_ = true;
    return r * std::cos(theta);
  }

  double normal(double mean, double sigma) { return mean + sigma * normal(); }

  bool bernoulli(double p) { return uniform() < p; }

 private:
  static std::uint64_t splitmix64(std::uint64_t& x) {
    std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

  std::uint64_t s_[4]{};
  double spare_ = 0.0;
  bool has_spare_ = false;
};

}  // namespace ipe
