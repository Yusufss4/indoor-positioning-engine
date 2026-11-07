#include "ipe/trace_player.hpp"

#include <algorithm>
#include <stdexcept>

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
                   p_ < tr.baro.size() ? tr.baro[p_].t : kNever, tr.truth[g_].t});
}

TracePlayer::Event TracePlayer::step(PositioningEngine& engine) {
  if (finished()) throw std::logic_error("TracePlayer::step after the end of the trace");
  const auto& tr = *trace_;
  const double ti = i_ < tr.imu.size() ? tr.imu[i_].t : kNever;
  const double tb = b_ < tr.ble.size() ? tr.ble[b_].t : kNever;
  const double tp = p_ < tr.baro.size() ? tr.baro[p_].t : kNever;
  const double tg = tr.truth[g_].t;

  if (ti <= tb && ti <= tp && ti <= tg) {
    engine.on_imu(tr.imu[i_++]);
    return Event::Imu;
  }
  if (tb <= tp && tb <= tg) {
    const std::size_t end = scan_end(tr.ble, b_);
    const std::size_t known = venue_->beacons().size();
    fed_ble_.clear();
    if (tr.ble[b_].scan != 0) {
      scan_.clear();
      for (std::size_t k = b_; k < end; ++k) {
        scan_.push_back({ble_index_[k], tr.ble[k].rssi});
        if (ble_index_[k] < known) fed_ble_.push_back(k);
      }
      engine.on_scan(tr.ble[b_].t, scan_);
    } else {
      engine.on_ble(tr.ble[b_].t, ble_index_[b_], tr.ble[b_].rssi);
      if (ble_index_[b_] < known) fed_ble_.push_back(b_);
    }
    b_ = end;
    return Event::Radio;
  }
  if (tp <= tg) {
    engine.on_pressure(tr.baro[p_].t, tr.baro[p_].hpa);
    ++p_;
    return Event::Pressure;
  }
  ++g_;
  return Event::Truth;
}

}  // namespace ipe
