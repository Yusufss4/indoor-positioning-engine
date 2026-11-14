#include "ipe/trace_player.hpp"

#include <algorithm>

namespace ipe {

TracePlayer::TracePlayer(const Venue& venue, const Trace& trace) : venue_(&venue), trace_(&trace) {
  ble_index_.reserve(trace.ble.size());
  for (const auto& b : trace.ble)
    ble_index_.push_back(venue.beacon_index(b.beacon_id).value_or(venue.beacons().size()));
}

double TracePlayer::next_time() const {
  if (finished()) return kNever;
  const auto& tr = *trace_;
  return std::min({i_ < tr.imu.size() ? tr.imu[i_].t : kNever, b_ < tr.ble.size() ? tr.ble[b_].t : kNever,
                   p_ < tr.baro.size() ? tr.baro[p_].t : kNever, x_ < tr.fixes.size() ? tr.fixes[x_].t : kNever,
                   tr.truth[g_].t});
}

}  // namespace ipe
