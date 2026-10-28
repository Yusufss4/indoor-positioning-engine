#pragma once

#include <optional>

namespace ipe {

struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

/// One inertial sample in the phone frame (phone held flat, x pointing in the
/// walking direction). Units: m/s^2, rad/s, microtesla.
struct ImuSample {
  double t = 0.0;
  Vec3 accel;
  Vec3 gyro;
  Vec3 mag;
};

struct StepEvent {
  double t = 0.0;
  double length = 0.0;         ///< Weinberg step length estimate [m].
  double heading_delta = 0.0;  ///< Gyro yaw change since the previous step [rad].
  double mag_heading = 0.0;    ///< Magnetometer heading averaged over the step [rad].
  bool mag_valid = false;
};

struct PdrConfig {
  double gravity = 9.80665;
  double lowpass_alpha = 0.25;      ///< EMA factor applied to |a| - g.
  double peak_threshold = 1.0;      ///< Peak must exceed this [m/s^2].
  double valley_threshold = 0.0;    ///< Signal must fall below this to confirm the peak.
  double min_step_interval = 0.25;  ///< Reject peaks closer than this [s].
  double weinberg_k = 0.48;
  double min_step_length = 0.25;
  double max_step_length = 1.2;
};

/// Detects steps from accelerometer magnitude peaks, estimates step length
/// with the Weinberg model L = K * (a_max - a_min)^(1/4) and integrates the
/// gyro z-axis to report the heading change between consecutive steps.
class PdrProcessor {
 public:
  explicit PdrProcessor(PdrConfig cfg = {}) : cfg_(cfg) {}

  /// Feeds one sample; returns a step event when a step is confirmed.
  std::optional<StepEvent> process(const ImuSample& s);

  [[nodiscard]] double integrated_yaw() const { return yaw_; }
  [[nodiscard]] const PdrConfig& config() const { return cfg_; }

  /// Heading of the walking direction from a flat-held magnetometer reading,
  /// measured counter-clockwise from the +x (east) axis with north along +y.
  static double heading_from_mag(const Vec3& mag);

 private:
  PdrConfig cfg_;
  bool initialized_ = false;
  double last_t_ = 0.0;
  double filtered_ = 0.0;
  double yaw_ = 0.0;
  double yaw_at_last_step_ = 0.0;
  double last_step_t_ = -1e9;

  bool in_peak_ = false;
  double peak_max_ = 0.0;
  double valley_min_ = 0.0;

  double mag_sin_sum_ = 0.0;
  double mag_cos_sum_ = 0.0;
  int mag_count_ = 0;
};

}  // namespace ipe
