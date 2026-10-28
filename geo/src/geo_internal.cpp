#include "geo_internal.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ipe::geo::detail {

namespace {

struct Line {
  Vec2 origin;
  Vec2 dir;  ///< Unit length.
  std::vector<std::pair<double, double>> spans;
};

}  // namespace

std::vector<Segment> merge_collinear(const std::vector<Segment>& segments, double tol) {
  std::vector<Line> lines;
  for (const auto& s : segments) {
    const Vec2 d = s.b - s.a;
    const double len = d.norm();
    if (len < 1e-6) continue;
    const Vec2 u = d * (1.0 / len);
    Line* match = nullptr;
    for (auto& l : lines) {
      if (std::abs(cross(l.dir, u)) > 0.01) continue;
      if (std::abs(cross(l.dir, s.a - l.origin)) > tol || std::abs(cross(l.dir, s.b - l.origin)) > tol) continue;
      match = &l;
      break;
    }
    if (!match) {
      lines.push_back({s.a, u, {}});
      match = &lines.back();
    }
    const double t0 = dot(s.a - match->origin, match->dir);
    const double t1 = dot(s.b - match->origin, match->dir);
    match->spans.emplace_back(std::min(t0, t1), std::max(t0, t1));
  }

  std::vector<Segment> out;
  for (auto& l : lines) {
    std::sort(l.spans.begin(), l.spans.end());
    auto cur = l.spans.front();
    const auto flush = [&] { out.push_back({l.origin + l.dir * cur.first, l.origin + l.dir * cur.second}); };
    for (std::size_t i = 1; i < l.spans.size(); ++i) {
      if (l.spans[i].first <= cur.second + 1e-6) {
        cur.second = std::max(cur.second, l.spans[i].second);
      } else {
        flush();
        cur = l.spans[i];
      }
    }
    flush();
  }
  return out;
}

std::vector<Segment> cut_openings(const std::vector<Segment>& walls, const std::vector<std::vector<Vec2>>& openings,
                                  double tol) {
  std::vector<Segment> out;
  for (const auto& w : walls) {
    const Vec2 d = w.b - w.a;
    const double len = d.norm();
    if (len < 1e-6) continue;
    const Vec2 u = d * (1.0 / len);
    std::vector<std::pair<double, double>> cuts;
    for (const auto& o : openings) {
      if (o.size() < 2) continue;
      double lo = 1e300, hi = -1e300;
      bool on_line = true;
      for (const Vec2& p : o) {
        if (std::abs(cross(u, p - w.a)) > tol) {
          on_line = false;
          break;
        }
        const double t = dot(p - w.a, u);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
      }
      lo = std::max(lo, 0.0);
      hi = std::min(hi, len);
      if (on_line && hi - lo > 1e-3) cuts.emplace_back(lo, hi);
    }
    std::sort(cuts.begin(), cuts.end());
    double start = 0.0;
    const auto emit = [&](double t0, double t1) {
      if (t1 - t0 >= 0.01) out.push_back({w.a + u * t0, w.a + u * t1});
    };
    for (const auto& [lo, hi] : cuts) {
      emit(start, lo);
      start = std::max(start, hi);
    }
    emit(start, len);
  }
  return out;
}

std::optional<Segment> thin_rect_centerline(const std::vector<Vec2>& ring_in, double max_thickness) {
  std::vector<Vec2> ring = ring_in;
  if (ring.size() == 5 && distance(ring.front(), ring.back()) < 1e-6) ring.pop_back();
  if (ring.size() != 4) return std::nullopt;
  Vec2 e[4];
  double len[4];
  for (int i = 0; i < 4; ++i) {
    e[i] = ring[static_cast<std::size_t>((i + 1) % 4)] - ring[static_cast<std::size_t>(i)];
    len[i] = e[i].norm();
    if (len[i] < 1e-6) return std::nullopt;
  }
  const double eps = 0.01;
  if (std::abs(len[0] - len[2]) > eps || std::abs(len[1] - len[3]) > eps) return std::nullopt;
  if (std::abs(dot(e[0], e[1])) > 0.02 * len[0] * len[1]) return std::nullopt;
  const auto mid = [&](int i) {
    return (ring[static_cast<std::size_t>(i)] + ring[static_cast<std::size_t>((i + 1) % 4)]) * 0.5;
  };
  if (len[1] <= len[0] && len[1] <= max_thickness) return Segment{mid(3), mid(1)};
  if (len[0] < len[1] && len[0] <= max_thickness) return Segment{mid(0), mid(2)};
  return std::nullopt;
}

std::vector<Vec2> wall_rect(const Segment& s, double thickness) {
  const Vec2 d = s.b - s.a;
  const double len = d.norm();
  const Vec2 n = len > 0.0 ? Vec2{-d.y / len, d.x / len} * (0.5 * thickness) : Vec2{};
  return {s.a + n, s.b + n, s.b - n, s.a - n, s.a + n};
}

double round_mm(double v) {
  const double r = std::round(v * 1000.0) / 1000.0;
  return r == 0.0 ? 0.0 : r;  // no "-0.0" in output
}

double round_deg(double v) { return std::round(v * 1e9) / 1e9; }

ojson position(const GeoRef& g, Vec2 p) {
  const LatLon ll = g.to_wgs84(p);
  return ojson::array({round_deg(ll.lon), round_deg(ll.lat)});
}

ojson polygon(const GeoRef& g, const std::vector<Vec2>& ring) {
  ojson coords = ojson::array();
  for (const Vec2& p : ring) coords.push_back(position(g, p));
  if (!ring.empty() && distance(ring.front(), ring.back()) > 1e-9) coords.push_back(position(g, ring.front()));
  return ojson{{"type", "Polygon"}, {"coordinates", ojson::array({coords})}};
}

std::string feature_collection(const std::vector<ojson>& features) {
  std::string out = "{\"type\": \"FeatureCollection\", \"features\": [\n";
  for (std::size_t i = 0; i < features.size(); ++i) {
    out += features[i].dump();
    out += i + 1 < features.size() ? ",\n" : "\n";
  }
  out += "]}\n";
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write " + path);
  out << text;
}

}  // namespace ipe::geo::detail
