#pragma once

namespace ipe {

/// International barometric formula (troposphere): altitude [m] for `hpa`. The reference pressure cancels
/// in the relative altitude the filter uses.
double pressure_to_altitude(double hpa, double sea_level_hpa = 1013.25);
/// Inverse of pressure_to_altitude.
double altitude_to_pressure(double altitude_m, double sea_level_hpa = 1013.25);

/// Turns a phone barometer stream into low-pass filtered altitude relative to
/// the first sample. Absolute altitude from a phone is meaningless (weather
/// moves it by metres per hour and devices disagree by several hPa), so the
/// filter only ever uses changes of this value plus its own offset states.
class BaroAltimeter {
 public:
  /// `lowpass_tau_s`: time constant of the pressure low-pass (door slams, HVAC gusts).
  explicit BaroAltimeter(double lowpass_tau_s = 1.0) : lowpass_tau_s_(lowpass_tau_s) {}

  /// Feeds one sample; returns the filtered relative altitude [m].
  double process(double t, double hpa);

  [[nodiscard]] double altitude() const { return filtered_; }

 private:
  double lowpass_tau_s_;
  bool initialized_ = false;
  double reference_m_ = 0.0;
  double filtered_ = 0.0;
  double last_t_ = 0.0;
};

}  // namespace ipe
