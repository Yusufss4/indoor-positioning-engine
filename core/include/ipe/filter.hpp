#pragma once

#include <cstddef>
#include <string_view>

#include "ipe/geometry.hpp"
#include "ipe/pdr.hpp"

namespace ipe {

struct Estimate {
  double t = 0.0;
  Vec2 pos;
  double heading = 0.0;
  double cov_xx = 0.0;
  double cov_xy = 0.0;
  double cov_yy = 0.0;
  int level = 0;   ///< Level index in the venue.
  double z = 0.0;  ///< Floor elevation of `level` [m].
  bool valid = false;

  /// Radius of a circle with the same area as the 1-sigma ellipse.
  [[nodiscard]] double sigma_radius() const;
};

/// Common interface so the replay tool, regression suite and web demo can
/// drive any estimator with the same measurement stream.
class Filter {
 public:
  virtual ~Filter() = default;

  virtual void on_step(const StepEvent& step) = 0;
  virtual void on_heading(double t, double heading, double sigma) = 0;
  virtual void on_rssi(double t, std::size_t beacon_index, double rssi) = 0;
  /// Barometric pressure [hPa]. Filters without a floor model ignore it.
  virtual void on_pressure(double /*t*/, double /*hpa*/) {}

  [[nodiscard]] virtual Estimate estimate() const = 0;
  [[nodiscard]] virtual std::string_view name() const = 0;
};

}  // namespace ipe
