#include "ipe/beacon_integrity.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {

BeaconIntegrityMonitor::BeaconIntegrityMonitor(std::size_t beacon_count, Settings settings)
    : settings_(settings), health_(beacon_count) {}

bool BeaconIntegrityMonitor::admit(std::size_t beacon, double residual) {
  auto& h = health_[beacon];
  h.mean_residual = h.samples == 0 ? residual : h.mean_residual + settings_.alpha * (residual - h.mean_residual);
  ++h.samples;

  if (!h.excluded && h.samples >= settings_.min_samples && std::abs(h.mean_residual) > settings_.threshold_db) {
    // Never drop more than a third of the infrastructure on our own say-so.
    const auto excluded = std::count_if(health_.begin(), health_.end(), [](const Health& b) { return b.excluded; });
    if (static_cast<std::size_t>(excluded + 1) * 3 <= health_.size()) h.excluded = true;
  } else if (h.excluded && std::abs(h.mean_residual) < 0.5 * settings_.threshold_db) {
    h.excluded = false;
  }
  return !h.excluded;
}

}  // namespace ipe
