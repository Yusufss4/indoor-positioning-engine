// WebAssembly entry point for the browser demo. The same C++ engine that the
// CLI tools use runs here unchanged; only this thin embind layer is web-specific.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ipe/engine.hpp"
#include "ipe/eval.hpp"
#include "ipe/simulator.hpp"
#include "ipe/trace.hpp"

using emscripten::val;

namespace {

struct Track {
  std::string name;
  std::unique_ptr<ipe::PositioningEngine> engine;
  double error_sum = 0.0;
  int error_count = 0;
  std::vector<double> errors;
  int floor_ok = 0;  ///< Fixes on the true level (horizontal error is only scored for these).
  int fixes = 0;
};

class Demo {
 public:
  /// Simulates the scenario and runs every filter with its preset config.
  Demo(const std::string& venue_json, const std::string& scenario_json, unsigned seed)
      : venue_(ipe::parse_venue(venue_json)) {
    trace_ = ipe::sim::simulate(ipe::sim::parse_scenario(scenario_json), venue_, seed);
    init("");
  }

  /// Replays a recorded trace. `pf_config_patch` (may be empty) applies to the particle filter only.
  static Demo* replay(const std::string& venue_json, const std::string& trace_csv, const std::string& pf_config_patch) {
    std::istringstream in(trace_csv);
    return new Demo(ipe::parse_venue(venue_json), ipe::read_trace(in), pf_config_patch);
  }

  double time() const { return now_; }
  double duration() const { return trace_.truth.empty() ? 0.0 : trace_.truth.back().t; }
  bool finished() const { return g_ >= trace_.truth.size(); }
  int levelCount() const { return venue_.level_count(); }

  /// Processes every sensor event up to now + dt. Returns false at the end.
  bool advance(double dt) {
    const double until = now_ + dt;
    recent_ble_.clear();
    while (g_ < trace_.truth.size()) {
      const double ti = i_ < trace_.imu.size() ? trace_.imu[i_].t : 1e300;
      const double tb = b_ < trace_.ble.size() ? trace_.ble[b_].t : 1e300;
      const double tp = p_ < trace_.baro.size() ? trace_.baro[p_].t : 1e300;
      const double tg = trace_.truth[g_].t;
      const double next = std::min({ti, tb, tp, tg});
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
      } else if (tp == next) {
        for (auto& tr : tracks_) tr.engine->on_pressure(trace_.baro[p_].t, trace_.baro[p_].hpa);
        ++p_;
      } else {
        truth_ = trace_.truth[g_++];
        for (auto& tr : tracks_) {
          const auto e = tr.engine->estimate();
          if (!e.valid || truth_.t < 5.0) continue;
          ++tr.fixes;
          if (e.level != truth_.level) continue;
          ++tr.floor_ok;
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

  /// Ground truth at the current time, interpolated between truth samples. Recorded traces only have
  /// sparse surveyor waypoints; scoring (mean, CEP95) still happens at the samples themselves.
  val truth() const {
    const auto t = truth_now();
    val o = val::object();
    o.set("x", t.pos.x);
    o.set("y", t.pos.y);
    o.set("heading", t.heading);
    o.set("level", t.level);
    o.set("z", t.z);
    set_wgs84(o, t.pos);
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
      o.set("level", e.level);
      o.set("z", e.z);
      set_wgs84(o, e.pos);
      o.set("sigma", e.sigma_radius());
      const auto t = truth_now();
      o.set("error", e.valid && e.level == t.level ? ipe::distance(e.pos, t.pos) : -1.0);
      o.set("mean", tr.error_count ? tr.error_sum / tr.error_count : -1.0);
      o.set("cep95", ipe::eval::percentile(tr.errors, 0.95));
      o.set("floorAcc", floor_accuracy(tr));
    }
    return o;
  }

  /// Fraction of fixes (after the 5 s warm-up) on the true level; -1 before the first.
  double floorAccuracy(const std::string& name) const {
    for (const auto& tr : tracks_) {
      if (tr.name == name) return floor_accuracy(tr);
    }
    return -1.0;
  }

  /// Flat [x0, y0, w0, level0, x1, ...] view of the particle cloud. The view
  /// aliases WASM memory and is only valid until the next call.
  val particles() {
    particle_buf_.clear();
    if (const auto* pf = tracks_.front().engine->particle_filter()) {
      for (const auto& p : pf->particles()) {
        particle_buf_.push_back(static_cast<float>(p.pos.x));
        particle_buf_.push_back(static_cast<float>(p.pos.y));
        particle_buf_.push_back(static_cast<float>(p.weight));
        particle_buf_.push_back(static_cast<float>(p.level));
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
  Demo(ipe::Venue venue, ipe::Trace trace, const std::string& pf_config_patch)
      : venue_(std::move(venue)), trace_(std::move(trace)) {
    init(pf_config_patch);
  }

  void init(const std::string& pf_config_patch) {
    for (const char* preset : {"pf", "ekf", "wcl"}) {
      Track tr;
      tr.name = preset;
      auto cfg = ipe::eval::preset_config(preset);
      if (tr.name == "pf" && !pf_config_patch.empty()) ipe::eval::apply_config_overrides(cfg, pf_config_patch);
      tr.engine = std::make_unique<ipe::PositioningEngine>(venue_, cfg);
      tracks_.push_back(std::move(tr));
    }
    ble_index_.reserve(trace_.ble.size());
    for (const auto& b : trace_.ble) ble_index_.push_back(venue_.beacon_index(b.beacon_id).value_or(SIZE_MAX));
  }

  ipe::TruthSample truth_now() const {
    if (g_ == 0 || g_ >= trace_.truth.size()) return truth_;
    const auto& next = trace_.truth[g_];
    if (next.level != truth_.level || next.t <= truth_.t) return truth_;
    const double a = std::clamp((now_ - truth_.t) / (next.t - truth_.t), 0.0, 1.0);
    auto t = truth_;
    t.pos = truth_.pos + (next.pos - truth_.pos) * a;
    return t;
  }

  static double floor_accuracy(const Track& tr) {
    return tr.fixes ? static_cast<double>(tr.floor_ok) / tr.fixes : -1.0;
  }

  void set_wgs84(val& o, ipe::Vec2 p) const {
    if (!venue_.georef()) return;
    const auto ll = venue_.georef()->to_wgs84(p);
    o.set("lat", ll.lat);
    o.set("lon", ll.lon);
  }

  ipe::Venue venue_;
  ipe::Trace trace_;
  std::vector<Track> tracks_;
  std::vector<std::size_t> ble_index_;
  std::vector<std::size_t> recent_ble_;
  std::vector<float> particle_buf_;
  ipe::TruthSample truth_{};
  double now_ = 0.0;
  std::size_t i_ = 0, b_ = 0, p_ = 0, g_ = 0;
};

}  // namespace

EMSCRIPTEN_BINDINGS(ipe) {
  emscripten::class_<Demo>("Demo")
      .constructor<std::string, std::string, unsigned>()
      .class_function("replay", &Demo::replay, emscripten::return_value_policy::take_ownership())
      .function("time", &Demo::time)
      .function("duration", &Demo::duration)
      .function("finished", &Demo::finished)
      .function("levelCount", &Demo::levelCount)
      .function("advance", &Demo::advance)
      .function("truth", &Demo::truth)
      .function("estimate", &Demo::estimate)
      .function("floorAccuracy", &Demo::floorAccuracy)
      .function("particles", &Demo::particles)
      .function("recentBle", &Demo::recentBle)
      .function("steps", &Demo::steps);
}
