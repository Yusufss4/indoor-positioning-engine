#include "ipe/fingerprint_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ipe {

FingerprintMap::FingerprintMap(const Venue& venue, FingerprintMapConfig cfg) : venue_(&venue), cfg_(cfg) {
  cfg_.k = std::clamp(cfg_.k, 1, kMaxK);
  cfg_.strongest = std::max(cfg_.strongest, 1);
  const auto& fp = venue.floor_fingerprints();
  fingerprinted_.assign(venue.beacons().size(), 0);
  for (const auto b : fp.beacon) fingerprinted_[b] = 1;
  scan_.assign(venue.beacons().size(), 0.0);
  compared_.assign(venue.beacons().size(), 0);
  touched_.reserve(venue.beacons().size());
}

int FingerprintMap::nearest(std::span<const RssiReading> scan, std::span<FingerprintNeighbour> out) {
  const auto& fp = venue_->floor_fingerprints();
  if (!fp.has_positions() || out.empty()) return 0;
  // Dense scan over fingerprinted beacons; a beacon heard twice keeps its strongest reading. RSSI is
  // negative, so 0 marks "not heard".
  touched_.clear();
  for (const auto& r : scan) {
    if (r.beacon_index >= scan_.size() || !fingerprinted_[r.beacon_index]) continue;
    const double rssi = std::min(r.rssi, -1e-6);
    if (scan_[r.beacon_index] == 0.0) {
      touched_.push_back(r.beacon_index);
      scan_[r.beacon_index] = rssi;
    } else {
      scan_[r.beacon_index] = std::max(scan_[r.beacon_index], rssi);
    }
  }
  const auto reset = [&] {
    for (const auto b : touched_) {
      scan_[b] = 0.0;
      compared_[b] = 0;
    }
  };
  if (static_cast<int>(touched_.size()) < cfg_.min_readings) {
    reset();
    return 0;
  }
  // The strongest readings; ties broken by beacon index so the choice does not depend on the scan's order.
  const auto n_compared = std::min(touched_.size(), static_cast<std::size_t>(cfg_.strongest));
  std::partial_sort(touched_.begin(), touched_.begin() + static_cast<std::ptrdiff_t>(n_compared), touched_.end(),
                    [&](std::size_t a, std::size_t b) { return scan_[a] != scan_[b] ? scan_[a] > scan_[b] : a < b; });
  // A fingerprint that heard none of them is at `base`; each one it did hear replaces its term.
  double base = 0.0;
  for (std::size_t i = 0; i < n_compared; ++i) {
    const auto b = touched_[i];
    compared_[b] = 1;
    const double miss = scan_[b] - cfg_.fill_dbm;
    base += miss * miss;
  }

  const int k = std::min({cfg_.k, static_cast<int>(fp.size()), static_cast<int>(out.size())});
  std::array<double, kMaxK> best_d{};
  std::array<std::size_t, kMaxK> best_i{};
  best_d.fill(std::numeric_limits<double>::infinity());
  for (std::size_t i = 0; i < fp.size(); ++i) {
    double d = base;
    for (auto r = fp.begin[i]; r < fp.begin[i + 1]; ++r) {
      const auto b = fp.beacon[r];
      if (!compared_[b]) continue;
      const double hit = scan_[b] - fp.rssi[r];
      const double miss = scan_[b] - cfg_.fill_dbm;
      d += hit * hit - miss * miss;
    }
    if (d >= best_d[static_cast<std::size_t>(k - 1)]) continue;
    int j = k - 1;
    while (j > 0 && best_d[static_cast<std::size_t>(j - 1)] > d) {
      best_d[static_cast<std::size_t>(j)] = best_d[static_cast<std::size_t>(j - 1)];
      best_i[static_cast<std::size_t>(j)] = best_i[static_cast<std::size_t>(j - 1)];
      --j;
    }
    best_d[static_cast<std::size_t>(j)] = d;
    best_i[static_cast<std::size_t>(j)] = i;
  }
  reset();

  double sum = 0.0;
  for (int j = 0; j < k; ++j) {
    const auto i = best_i[static_cast<std::size_t>(j)];
    auto& n = out[static_cast<std::size_t>(j)];
    n.pos = fp.pos[i];
    n.level = fp.level[i];
    n.distance_db = std::sqrt(std::max(best_d[static_cast<std::size_t>(j)], 0.0) / static_cast<double>(n_compared));
    n.weight = 1.0 / (n.distance_db + 1.0);
    sum += n.weight;
  }
  for (int j = 0; j < k; ++j) out[static_cast<std::size_t>(j)].weight /= sum;
  return k;
}

}  // namespace ipe
