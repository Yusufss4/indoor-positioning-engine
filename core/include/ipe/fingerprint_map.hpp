#pragma once

#include <span>
#include <vector>

#include "ipe/estimate.hpp"
#include "ipe/venue.hpp"

namespace ipe {

struct FingerprintMapConfig {
  int k = 10;                ///< Neighbours returned.
  int strongest = 20;        ///< Readings of the scan that are compared: its strongest ones.
  double fill_dbm = -100.0;  ///< RSSI assumed for a compared beacon that a fingerprint did not hear.
  int min_readings = 3;      ///< Scans with fewer readings of fingerprinted beacons give no neighbours.
};

/// A training scan close to the current one in signal space.
struct FingerprintNeighbour {
  Vec2 pos;
  int level = 0;
  double distance_db = 0.0;  ///< RMS difference over the compared readings [dB].
  double weight = 0.0;       ///< Share among the neighbours returned; they sum to 1.
};

/// Nearest training scans of a scan, over the venue's fingerprints that carry positions. Only the scan's
/// strongest readings are compared (weak ones come and go with the receiver, and a fingerprint that
/// merely shares the far access points is not near). Cost per scan is O(total fingerprint readings);
/// nothing is allocated after construction.
class FingerprintMap {
 public:
  static constexpr int kMaxK = 32;

  FingerprintMap(const Venue& venue, FingerprintMapConfig cfg);

  /// True when the venue's fingerprints have positions.
  [[nodiscard]] bool available() const { return venue_->floor_fingerprints().has_positions(); }
  /// Writes the nearest fingerprints to `out`, nearest first, and returns how many (at most `k` and
  /// `out.size()`); 0 when there are no positions or too few readings of fingerprinted beacons.
  /// Neighbour weights are proportional to 1 / (distance + 1 dB).
  int nearest(std::span<const RssiReading> scan, std::span<FingerprintNeighbour> out);

 private:
  const Venue* venue_;
  FingerprintMapConfig cfg_;
  std::vector<char> fingerprinted_;  ///< Per beacon, appears in some fingerprint.
  std::vector<double> scan_;         ///< Per beacon, the scan's reading (strongest if repeated); 0 = not heard.
  std::vector<char> compared_;       ///< Per beacon, among the scan's strongest.
  std::vector<std::size_t> touched_;
};

}  // namespace ipe
