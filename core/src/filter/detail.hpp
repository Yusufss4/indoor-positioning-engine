#pragma once

// Helpers shared by the grid filter's source files (core/src/filter).

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ipe::filter_detail {

inline constexpr double kTwoPi = 2.0 * std::numbers::pi;
inline constexpr double kMinVar = 1e-6;
/// Cells under this share of the level's heaviest cell are left out of the map-consistent estimate: every
/// cell holds some recovery mass, and that mass is not a group of hypotheses.
inline constexpr double kPointFloorRel = 1e-3;

/// Share of the uniform prior mixed in before a position fix is applied (position_fix.cpp).
inline constexpr double kFixPriorShare = 0.01;

/// std::floor to int, for |v| < 2^31.
inline int floor_int(double v) {
  const int i = static_cast<int>(v);
  return i - (v < static_cast<double>(i));
}

/// Mixes N(mu2, var2) into N(mu, var) with weight f (moment matching).
inline void mix_gaussian(double& mu, double& var, double mu2, double var2, double f) {
  const double m = (1.0 - f) * mu + f * mu2;
  const double e2 = (1.0 - f) * (var + mu * mu) + f * (var2 + mu2 * mu2);
  mu = m;
  var = std::max(e2 - m * m, kMinVar);
}

/// Robust Gaussian/uniform likelihood of residual `r` with predictive variance `s2`; `resp` gets the inlier
/// responsibility.
inline double mixture(double r, double s2, double outlier_prob, double outlier_density, double& resp) {
  const double g = (1.0 - outlier_prob) * std::exp(-0.5 * r * r / s2) / std::sqrt(kTwoPi * s2);
  const double lik = g + outlier_prob * outlier_density;
  resp = lik > 0.0 ? g / lik : 0.0;
  return lik;
}

/// Assumed-density update of N(mu, var) from a reading z = x + mu + noise(noise_var), with residual
/// `r` = z - x - mu and inlier responsibility `resp`.
inline void adf_update(double& mu, double& var, double r, double noise_var, double resp) {
  const double s2 = noise_var + var;
  const double gain = var / s2;
  const double shift = gain * r;
  const double post_var = var * (1.0 - gain);
  mu += resp * shift;
  var = std::max(resp * post_var + (1.0 - resp) * var + resp * (1.0 - resp) * shift * shift, kMinVar);
}

}  // namespace ipe::filter_detail
