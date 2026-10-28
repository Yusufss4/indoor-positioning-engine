#include "ipe/trace.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string_view>

namespace ipe {
namespace {

std::vector<std::string_view> split(std::string_view line, char sep) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (true) {
    const auto pos = line.find(sep, start);
    out.push_back(line.substr(start, pos - start));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

double to_double(std::string_view s, std::size_t line_no) {
  double v = 0.0;
  const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
  if (res.ec != std::errc{}) {
    throw std::runtime_error("trace line " + std::to_string(line_no) + ": bad number '" + std::string(s) + "'");
  }
  return v;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

}  // namespace

double Trace::duration() const {
  double t0 = 1e300, t1 = -1e300;
  auto span = [&](double t) {
    t0 = std::min(t0, t);
    t1 = std::max(t1, t);
  };
  if (!imu.empty()) span(imu.front().t), span(imu.back().t);
  if (!ble.empty()) span(ble.front().t), span(ble.back().t);
  if (!truth.empty()) span(truth.front().t), span(truth.back().t);
  return t1 > t0 ? t1 - t0 : 0.0;
}

void write_trace(std::ostream& out, const Trace& trace) {
  out << "# ipe-trace v1\n";
  for (const auto& [k, v] : trace.metadata) out << "# " << k << ": " << v << "\n";
  out << "# imu,t,ax,ay,az,gx,gy,gz,mx,my,mz\n# ble,t,beacon_id,rssi\n# gt,t,x,y,heading\n";

  // Merge the three streams by timestamp so the file reads like a live log.
  std::size_t i = 0, b = 0, g = 0;
  char buf[256];
  while (i < trace.imu.size() || b < trace.ble.size() || g < trace.truth.size()) {
    const double ti = i < trace.imu.size() ? trace.imu[i].t : 1e300;
    const double tb = b < trace.ble.size() ? trace.ble[b].t : 1e300;
    const double tg = g < trace.truth.size() ? trace.truth[g].t : 1e300;
    if (ti <= tb && ti <= tg) {
      const auto& s = trace.imu[i++];
      std::snprintf(buf, sizeof(buf), "imu,%.3f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.3f,%.3f,%.3f\n", s.t, s.accel.x,
                    s.accel.y, s.accel.z, s.gyro.x, s.gyro.y, s.gyro.z, s.mag.x, s.mag.y, s.mag.z);
    } else if (tb <= tg) {
      const auto& s = trace.ble[b++];
      std::snprintf(buf, sizeof(buf), "ble,%.3f,%s,%.0f\n", s.t, s.beacon_id.c_str(), s.rssi);
    } else {
      const auto& s = trace.truth[g++];
      std::snprintf(buf, sizeof(buf), "gt,%.3f,%.3f,%.3f,%.4f\n", s.t, s.pos.x, s.pos.y, s.heading);
    }
    out << buf;
  }
}

void write_trace_file(const std::string& path, const Trace& trace) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write trace file: " + path);
  write_trace(out, trace);
}

Trace read_trace(std::istream& in) {
  Trace trace;
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const std::string_view sv = trim(line);
    if (sv.empty()) continue;
    if (sv.front() == '#') {
      const auto body = trim(sv.substr(1));
      const auto colon = body.find(':');
      if (colon != std::string_view::npos && body.substr(0, colon).find(',') == std::string_view::npos) {
        trace.metadata[std::string(trim(body.substr(0, colon)))] = std::string(trim(body.substr(colon + 1)));
      }
      continue;
    }
    const auto f = split(sv, ',');
    const auto bad = [&] { return std::runtime_error("trace line " + std::to_string(line_no) + ": malformed row"); };
    if (f[0] == "imu") {
      if (f.size() != 11) throw bad();
      ImuSample s;
      s.t = to_double(f[1], line_no);
      s.accel = {to_double(f[2], line_no), to_double(f[3], line_no), to_double(f[4], line_no)};
      s.gyro = {to_double(f[5], line_no), to_double(f[6], line_no), to_double(f[7], line_no)};
      s.mag = {to_double(f[8], line_no), to_double(f[9], line_no), to_double(f[10], line_no)};
      trace.imu.push_back(s);
    } else if (f[0] == "ble") {
      if (f.size() != 4) throw bad();
      trace.ble.push_back({to_double(f[1], line_no), std::string(f[2]), to_double(f[3], line_no)});
    } else if (f[0] == "gt") {
      if (f.size() != 5) throw bad();
      trace.truth.push_back(
          {to_double(f[1], line_no), {to_double(f[2], line_no), to_double(f[3], line_no)}, to_double(f[4], line_no)});
    } else {
      throw std::runtime_error("trace line " + std::to_string(line_no) + ": unknown row type '" + std::string(f[0]) +
                               "'");
    }
  }

  auto by_time = [](const auto& a, const auto& b) { return a.t < b.t; };
  std::stable_sort(trace.imu.begin(), trace.imu.end(), by_time);
  std::stable_sort(trace.ble.begin(), trace.ble.end(), by_time);
  std::stable_sort(trace.truth.begin(), trace.truth.end(), by_time);
  return trace;
}

Trace read_trace_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open trace file: " + path);
  return read_trace(in);
}

}  // namespace ipe
