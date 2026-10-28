// WebAssembly entry point for the browser demo. The same C++ engine that the
// CLI tools use runs here unchanged; only this thin embind layer is web-specific.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/eval.hpp"
#include "ipe/simulator.hpp"

using emscripten::val;

namespace {

struct Track {
  std::string name;
  std::unique_ptr<ipe::PositioningEngine> engine;
  double error_sum = 0.0;
  int error_count = 0;
  std::vector<double> errors;
};

class Demo {
 public:
  Demo(const std::string& venue_json, const std::string& scenario_json, unsigned seed)
      : venue_(ipe::parse_venue(venue_json)) {
    const auto scenario = ipe::sim::parse_scenario(scenario_json);
    trace_ = ipe::sim::simulate(scenario, venue_, seed);
    for (const char* preset : {"pf", "ekf", "wcl"}) {
      Track tr;
      tr.name = preset;
      tr.engine = std::make_unique<ipe::PositioningEngine>(venue_, ipe::eval::preset_config(preset));
      tracks_.push_back(std::move(tr));
    }
    ble_index_.reserve(trace_.ble.size());
    for (const auto& b : trace_.ble) ble_index_.push_back(venue_.beacon_index(b.beacon_id).value_or(SIZE_MAX));
  }

  double time() const { return now_; }
  double duration() const { return trace_.truth.empty() ? 0.0 : trace_.truth.back().t; }
  bool finished() const { return g_ >= trace_.truth.size(); }

  /// Processes every sensor event up to now + dt. Returns false at the end.
  bool advance(double dt) {
    const double until = now_ + dt;
    recent_ble_.clear();
    while (g_ < trace_.truth.size()) {
      const double ti = i_ < trace_.imu.size() ? trace_.imu[i_].t : 1e300;
      const double tb = b_ < trace_.ble.size() ? trace_.ble[b_].t : 1e300;
      const double tg = trace_.truth[g_].t;
      const double next = std::min({ti, tb, tg});
      if (next > until) break;
      if (ti == next) {
        for (auto& tr : tracks_) tr.engine->on_imu(trace_.imu[i_]);
        ++i_;
      } else if (tb == next) {
        if (ble_index_[b_] != SIZE_MAX) {
          for (auto& tr : tracks_) tr.engine->on_ble(trace_.ble[b_].t, ble_index_[b_], trace_.ble[b_].rssi);
          recent_ble_.push_back(b_);
        }
        ++b_;
      } else {
        truth_ = trace_.truth[g_++];
        for (auto& tr : tracks_) {
          const auto e = tr.engine->estimate();
          if (!e.valid || truth_.t < 5.0) continue;
          const double err = ipe::distance(e.pos, truth_.pos);
          tr.error_sum += err;
          ++tr.error_count;
          tr.errors.push_back(err);
        }
      }
    }
    now_ = until;
    return !finished();
  }

  val truth() const {
    val o = val::object();
    o.set("x", truth_.pos.x);
    o.set("y", truth_.pos.y);
    o.set("heading", truth_.heading);
    return o;
  }

  val estimate(const std::string& name) const {
    val o = val::object();
    for (const auto& tr : tracks_) {
      if (tr.name != name) continue;
      const auto e = tr.engine->estimate();
      o.set("valid", e.valid);
      o.set("x", e.pos.x);
      o.set("y", e.pos.y);
      o.set("sigma", e.sigma_radius());
      o.set("error", e.valid ? ipe::distance(e.pos, truth_.pos) : -1.0);
      o.set("mean", tr.error_count ? tr.error_sum / tr.error_count : -1.0);
      o.set("cep95", ipe::eval::percentile(tr.errors, 0.95));
    }
    return o;
  }

  /// Flat [x0, y0, w0, x1, y1, w1, ...] view of the particle cloud. The view
  /// aliases WASM memory and is only valid until the next call.
  val particles() {
    particle_buf_.clear();
    if (const auto* pf = tracks_.front().engine->particle_filter()) {
      for (const auto& p : pf->particles()) {
        particle_buf_.push_back(static_cast<float>(p.pos.x));
        particle_buf_.push_back(static_cast<float>(p.pos.y));
        particle_buf_.push_back(static_cast<float>(p.weight));
      }
    }
    return val(emscripten::typed_memory_view(particle_buf_.size(), particle_buf_.data()));
  }

  /// Beacon packets received during the last advance(): [{id, rssi}].
  val recentBle() const {
    val arr = val::array();
    for (std::size_t k : recent_ble_) {
      val o = val::object();
      o.set("id", trace_.ble[k].beacon_id);
      o.set("rssi", trace_.ble[k].rssi);
      arr.call<void>("push", o);
    }
    return arr;
  }

  int steps() const { return static_cast<int>(tracks_.front().engine->step_count()); }

 private:
  ipe::Venue venue_;
  ipe::Trace trace_;
  std::vector<Track> tracks_;
  std::vector<std::size_t> ble_index_;
  std::vector<std::size_t> recent_ble_;
  std::vector<float> particle_buf_;
  ipe::TruthSample truth_{};
  double now_ = 0.0;
  std::size_t i_ = 0, b_ = 0, g_ = 0;
};

}  // namespace

EMSCRIPTEN_BINDINGS(ipe) {
  emscripten::class_<Demo>("Demo")
      .constructor<std::string, std::string, unsigned>()
      .function("time", &Demo::time)
      .function("duration", &Demo::duration)
      .function("finished", &Demo::finished)
      .function("advance", &Demo::advance)
      .function("truth", &Demo::truth)
      .function("estimate", &Demo::estimate)
      .function("particles", &Demo::particles)
      .function("recentBle", &Demo::recentBle)
      .function("steps", &Demo::steps);
}
