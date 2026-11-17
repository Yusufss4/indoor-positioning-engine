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

// The filter's constants: values no deployment and no test changes. Changing one is a change of the
// filter, measured like any other (AGENTS.md).

/// Share of a magnetometer heading that is taken as wrong whatever it reads (a field bent by steel).
inline constexpr double kHeadingOutlierProb = 0.15;
/// Random walk of a cell's device RSSI offset per RSSI update [dB].
inline constexpr double kRssiOffsetWalk = 0.05;
/// Recovery mass is mixed in at most this often [s].
inline constexpr double kTeleportPeriodS = 0.5;
/// Barometer: an update at most this often [s], with this share of a connector cell's mass changing level,
/// this spread of the altitude [m], this share of outliers, and this random walk of a cell's offset [m].
inline constexpr double kBaroUpdateIntervalS = 0.5;
inline constexpr double kLevelChangeProbBaro = 0.1;
inline constexpr double kBaroSigmaM = 0.8;
inline constexpr double kBaroOutlierProb = 0.05;
inline constexpr double kBaroOffsetWalkM = 0.05;
/// Beacon integrity: a beacon is distrusted when its averaged residual (factor `alpha`, after this many
/// samples, while the estimate's spread is under this many metres) exceeds the threshold [dB].
inline constexpr double kIntegrityThresholdDb = 10.0;
inline constexpr double kIntegrityAlpha = 0.1;
inline constexpr int kIntegrityMinSamples = 8;
inline constexpr double kIntegrityMaxSigmaM = 3.0;
/// Floor vote: neighbours of the k-NN classifier, and the share of the uniform prior mixed into its vote.
inline constexpr int kFloorPriorK = 5;
inline constexpr double kFloorPriorEps = 0.1;
/// Share of the uniform prior mixed into the fingerprint density.
inline constexpr double kFingerprintEps = 0.05;

/// A background block is updated cell by cell again when its mass per cell reaches this many times the
/// active floor: the gap keeps a block at the floor from being folded and unfolded at every update.
inline constexpr double kActivateRatio = 16.0;
/// Upper bound of GridFilterConfig::active_floor.
inline constexpr double kMaxActiveFloor = 1e-3;

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
