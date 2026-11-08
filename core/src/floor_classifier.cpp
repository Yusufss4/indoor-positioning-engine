#include "ipe/floor_classifier.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace ipe {

FloorClassifier::FloorClassifier(const Venue& venue, FloorClassifierConfig cfg) : venue_(&venue), cfg_(cfg) {
  cfg_.k = std::clamp(cfg_.k, 1, kMaxK);
  const auto& fp = venue.floor_fingerprints();
  norm2_.assign(fp.size(), 0.0);
  fingerprinted_.assign(venue.beacons().size(), 0);
  for (std::size_t i = 0; i < fp.size(); ++i) {
    for (auto r = fp.begin[i]; r < fp.begin[i + 1]; ++r) {
      const double a = fp.rssi[r] - cfg_.fill_dbm;
      norm2_[i] += a * a;
      fingerprinted_[fp.beacon[r]] = 1;
    }
  }
  scan_.assign(venue.beacons().size(), 0.0);
  touched_.reserve(venue.beacons().size());
}

bool FloorClassifier::classify(std::span<const RssiReading> scan, std::span<double> out) {
  const auto& fp = venue_->floor_fingerprints();
  if (fp.empty() || out.size() < static_cast<std::size_t>(venue_->level_count())) return false;
  // Dense scan vector over fingerprinted beacons; a beacon heard twice keeps its strongest reading.
  touched_.clear();
  double scan_norm2 = 0.0;
  for (const auto& r : scan) {
    if (r.beacon_index >= scan_.size() || !fingerprinted_[r.beacon_index]) continue;
    const double a = std::max(r.rssi - cfg_.fill_dbm, 0.0);
    if (scan_[r.beacon_index] == 0.0) touched_.push_back(r.beacon_index);
    scan_[r.beacon_index] = std::max(scan_[r.beacon_index], a);
  }
  if (touched_.empty()) return false;
  for (const auto b : touched_) scan_norm2 += scan_[b] * scan_[b];

  // |s - f|^2 = |s|^2 + |f|^2 - 2 s.f, with s.f over the fingerprint's own readings only.
  const int k = std::min(cfg_.k, static_cast<int>(fp.size()));
  std::array<double, kMaxK> best_d{};
  std::array<int, kMaxK> best_level{};
  best_d.fill(std::numeric_limits<double>::infinity());
  for (std::size_t i = 0; i < fp.size(); ++i) {
    double dot = 0.0;
    for (auto r = fp.begin[i]; r < fp.begin[i + 1]; ++r) dot += scan_[fp.beacon[r]] * (fp.rssi[r] - cfg_.fill_dbm);
    const double d = scan_norm2 + norm2_[i] - 2.0 * dot;
    if (d >= best_d[static_cast<std::size_t>(k - 1)]) continue;
    int j = k - 1;
    while (j > 0 && best_d[static_cast<std::size_t>(j - 1)] > d) {
      best_d[static_cast<std::size_t>(j)] = best_d[static_cast<std::size_t>(j - 1)];
      best_level[static_cast<std::size_t>(j)] = best_level[static_cast<std::size_t>(j - 1)];
      --j;
    }
    best_d[static_cast<std::size_t>(j)] = d;
    best_level[static_cast<std::size_t>(j)] = fp.level[i];
  }
  for (const auto b : touched_) scan_[b] = 0.0;

  const int levels = venue_->level_count();
  const double eps = std::clamp(cfg_.eps, 0.0, 1.0);
  for (int l = 0; l < levels; ++l) out[static_cast<std::size_t>(l)] = eps / levels;
  for (int j = 0; j < k; ++j) out[static_cast<std::size_t>(best_level[static_cast<std::size_t>(j)])] += (1.0 - eps) / k;
  return true;
}

}  // namespace ipe
