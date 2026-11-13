#pragma once

#include <span>
#include <vector>

#include "ipe/estimate.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct FloorClassifierConfig {
  int k = 5;                 ///< Neighbours that vote.
  double eps = 0.1;          ///< Probability mass spread evenly over the levels (k-NN is sometimes wrong).
  double fill_dbm = -100.0;  ///< RSSI assumed for a beacon missing from a scan or a fingerprint.
};

/// k-nearest-neighbour floor classifier over the venue's floor fingerprints: Euclidean distance in RSSI
/// space, missing beacons at `fill_dbm`. Cost per scan is O(total fingerprint readings); nothing is
/// allocated after construction.
class FloorClassifier {
 public:
  static constexpr int kMaxK = 16;

  FloorClassifier(const Venue& venue, FloorClassifierConfig cfg);

  [[nodiscard]] bool available() const { return !venue_->floor_fingerprints().empty(); }
  /// Level probabilities for one scan, (1 - eps) * votes / k + eps / levels. Returns false (and leaves
  /// `out` alone) when there are no fingerprints or no reading of a fingerprinted beacon.
  bool classify(std::span<const RssiReading> scan, std::span<double> out);

 private:
  const Venue* venue_;
  FloorClassifierConfig cfg_;
  std::vector<double> norm2_;        ///< Per fingerprint, sum of (rssi - fill)^2.
  std::vector<char> fingerprinted_;  ///< Per beacon, appears in some fingerprint.
  std::vector<double> scan_;         ///< Dense (rssi - fill) of the current scan, per beacon.
  std::vector<std::size_t> touched_;
};

}  // namespace ipe
