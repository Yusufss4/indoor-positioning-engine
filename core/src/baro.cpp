#include "ipe/baro.hpp"

#include <algorithm>
#include <cmath>

namespace ipe {
namespace {
constexpr double kScaleHeight = 44330.0;
constexpr double kExponent = 1.0 / 5.255;
}  // namespace

double pressure_to_altitude(double hpa, double sea_level_hpa) {
  return kScaleHeight * (1.0 - std::pow(hpa / sea_level_hpa, kExponent));
}

double altitude_to_pressure(double altitude_m, double sea_level_hpa) {
  return sea_level_hpa * std::pow(1.0 - altitude_m / kScaleHeight, 5.255);
}

double BaroAltimeter::process(double t, double hpa) {
  const double h = pressure_to_altitude(hpa, cfg_.sea_level_hpa);
  if (!initialized_) {
    initialized_ = true;
    reference_m_ = h;
    filtered_ = 0.0;
    last_t_ = t;
    return filtered_;
  }
  const double dt = std::max(0.0, t - last_t_);
  last_t_ = t;
  const double alpha = cfg_.lowpass_tau_s > 0.0 ? dt / (cfg_.lowpass_tau_s + dt) : 1.0;
  filtered_ += alpha * ((h - reference_m_) - filtered_);
  return filtered_;
}

}  // namespace ipe
