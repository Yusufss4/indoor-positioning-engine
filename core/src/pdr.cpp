#include "ipe/pdr.hpp"

#include <algorithm>
#include <cmath>

#include "ipe/geometry.hpp"

namespace ipe {

double PdrProcessor::heading_from_mag(const Vec3& mag) {
  // Phone x axis along the walking direction; the horizontal field points
  // north (+y). In the phone frame the field reads (B sin psi, B cos psi).
  return std::atan2(mag.x, mag.y);
}

std::optional<StepEvent> PdrProcessor::process(const ImuSample& s) {
  const double a_norm = std::sqrt(s.accel.x * s.accel.x + s.accel.y * s.accel.y + s.accel.z * s.accel.z);
  const double a_dyn = a_norm - cfg_.gravity;

  if (!initialized_) {
    initialized_ = true;
    last_t_ = s.t;
    filtered_ = a_dyn;
    valley_min_ = a_dyn;
  }

  const double dt = std::clamp(s.t - last_t_, 0.0, 0.2);
  last_t_ = s.t;
  yaw_ += s.gyro.z * dt;
  filtered_ += cfg_.lowpass_alpha * (a_dyn - filtered_);

  const double mag_psi = heading_from_mag(s.mag);
  mag_sin_sum_ += std::sin(mag_psi);
  mag_cos_sum_ += std::cos(mag_psi);
  ++mag_count_;

  if (!in_peak_) {
    valley_min_ = std::min(valley_min_, filtered_);
    if (filtered_ > cfg_.peak_threshold) {
      in_peak_ = true;
      peak_max_ = filtered_;
    }
    return std::nullopt;
  }

  peak_max_ = std::max(peak_max_, filtered_);
  if (filtered_ >= cfg_.valley_threshold) return std::nullopt;

  in_peak_ = false;
  const double amplitude = peak_max_ - valley_min_;
  valley_min_ = filtered_;
  if (s.t - last_step_t_ < cfg_.min_step_interval) return std::nullopt;

  StepEvent ev;
  ev.t = s.t;
  ev.length = std::clamp(cfg_.weinberg_k * std::pow(std::max(amplitude, 0.0), 0.25), cfg_.min_step_length,
                         cfg_.max_step_length);
  ev.heading_delta = wrap_angle(yaw_ - yaw_at_last_step_);
  if (mag_count_ > 0) {
    ev.mag_heading = std::atan2(mag_sin_sum_, mag_cos_sum_);
    ev.mag_valid = true;
  }

  yaw_at_last_step_ = yaw_;
  last_step_t_ = s.t;
  mag_sin_sum_ = mag_cos_sum_ = 0.0;
  mag_count_ = 0;
  return ev;
}

}  // namespace ipe
