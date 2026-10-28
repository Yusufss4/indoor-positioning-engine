#pragma once

namespace ipe {

struct BaroConfig {
  double lowpass_tau_s = 1.0;      ///< Time constant of the pressure low-pass (door slams, HVAC gusts).
  double sea_level_hpa = 1013.25;  ///< Reference for the absolute-altitude formula; cancels in relative altitude.
};

/// International barometric formula (troposphere): altitude [m] for `hpa`.
double pressure_to_altitude(double hpa, double sea_level_hpa = 1013.25);
/// Inverse of pressure_to_altitude.
double altitude_to_pressure(double altitude_m, double sea_level_hpa = 1013.25);

/// Turns a phone barometer stream into low-pass filtered altitude relative to
/// the first sample. Absolute altitude from a phone is meaningless (weather
/// moves it by metres per hour and devices disagree by several hPa), so the
/// filters only ever use changes of this value plus their own offset states.
class BaroAltimeter {
 public:
  explicit BaroAltimeter(BaroConfig cfg = {}) : cfg_(cfg) {}

  /// Feeds one sample; returns the filtered relative altitude [m].
  double process(double t, double hpa);

  [[nodiscard]] bool initialized() const { return initialized_; }
  [[nodiscard]] double altitude() const { return filtered_; }
  [[nodiscard]] const BaroConfig& config() const { return cfg_; }

 private:
  BaroConfig cfg_;
  bool initialized_ = false;
  double reference_m_ = 0.0;
  double filtered_ = 0.0;
  double last_t_ = 0.0;
};

}  // namespace ipe
