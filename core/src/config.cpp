#include "ipe/config.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace ipe {

namespace {

/// Converts to anything: lets field_count try aggregate initialisation with one more initialiser.
struct AnyField {
  template <class T>
  operator T() const;  // NOLINT(google-explicit-constructor)
};

/// Number of fields of the aggregate T.
template <class T, class... A>
consteval std::size_t field_count(A... a) {
  if constexpr (requires { T{a..., AnyField{}}; }) {
    return field_count<T>(a..., AnyField{});
  } else {
    return sizeof...(A);
  }
}

/// Number of names in a macro's stringified argument list.
consteval std::size_t name_count(std::string_view names) {
  return names.empty() ? 0 : 1 + static_cast<std::size_t>(std::count(names.begin(), names.end(), ','));
}

}  // namespace

/// Registers a config struct's fields for JSON. A field that is missing from the list does not compile:
/// it would otherwise keep its default whatever a patch says.
#define IPE_CONFIG_JSON(Type, ...)                                                                         \
  static_assert(field_count<Type>() == name_count(#__VA_ARGS__),                                           \
                #Type ": list every field here, or config patches cannot reach it (core/src/config.cpp)"); \
  NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Type, __VA_ARGS__)

IPE_CONFIG_JSON(PdrConfig, gravity, lowpass_alpha, peak_threshold, valley_threshold, min_step_interval, weinberg_k,
                min_step_length, max_step_length)
IPE_CONFIG_JSON(RssiModelConfig, sigma_db, outlier_prob, outlier_range_db, min_distance, wall_aware)
IPE_CONFIG_JSON(BaroConfig, lowpass_tau_s, sea_level_hpa)
IPE_CONFIG_JSON(GridFilterConfig, cell_m, max_cells, max_reach_m, closed_region_fraction, closed_region_min_m2,
                heading_bins, heading_sigma_deg, heading_outlier_prob, step_length_sigma, step_length_rel_sigma,
                estimate_rssi_offset, rssi_offset_sigma, rssi_offset_walk, teleport_rate_per_s, teleport_period_s,
                level_change_prob_step, level_change_prob_baro, baro_update_interval_s, baro_sigma_m, baro_outlier_prob,
                baro_offset_sigma_m, baro_offset_walk_m, level_smoothing_s, level_hysteresis, map_consistent_estimate,
                beacon_integrity, integrity_threshold_db, integrity_alpha, integrity_min_samples, integrity_max_sigma_m,
                floor_prior_weight, floor_prior_k, floor_prior_eps, fingerprint_weight, fingerprint_k,
                fingerprint_strongest, fingerprint_sigma_m, fingerprint_eps, fingerprint_model_weight)
IPE_CONFIG_JSON(TrackSmootherConfig, enabled, tau_s, max_correction_mps, snap_m, extrapolate_steps, max_step_interval_s)
IPE_CONFIG_JSON(RoomEventConfig, enter_prob, exit_prob, enter_dwell_s, exit_dwell_s, update_period_s)

// The sections of EngineConfig. A new section goes into both functions below and into this count.
static_assert(field_count<EngineConfig>() == 7, "EngineConfig: register the new field in to_json and from_json");

void to_json(nlohmann::json& j, const EngineConfig& c) {
  j = nlohmann::json{{"pdr", c.pdr},
                     {"rssi", c.rssi},
                     {"baro", c.baro},
                     {"filter", c.filter},
                     {"rooms", c.rooms},
                     {"smoothing", c.smoothing},
                     {"mag_heading_sigma_deg", c.mag_heading_sigma_deg}};
}

void from_json(const nlohmann::json& j, EngineConfig& c) {
  j.at("pdr").get_to(c.pdr);
  j.at("rssi").get_to(c.rssi);
  if (j.contains("baro")) j.at("baro").get_to(c.baro);
  j.at("filter").get_to(c.filter);
  if (j.contains("rooms")) j.at("rooms").get_to(c.rooms);
  if (j.contains("smoothing")) j.at("smoothing").get_to(c.smoothing);
  j.at("mag_heading_sigma_deg").get_to(c.mag_heading_sigma_deg);
}

namespace {

void check_known_keys(const nlohmann::json& base, const nlohmann::json& patch, const std::string& path) {
  if (!patch.is_object()) return;
  for (const auto& [key, value] : patch.items()) {
    const std::string full = path.empty() ? key : path + "." + key;
    if (!base.is_object() || !base.contains(key)) throw std::invalid_argument("unknown config key '" + full + "'");
    // A section given as a scalar would otherwise fail deep inside the JSON library.
    if (base.at(key).is_object() && !value.is_object() && !value.is_null()) {
      throw std::invalid_argument("config key '" + full + "' is a section and needs an object");
    }
    check_known_keys(base.at(key), value, full);
  }
}

}  // namespace

void apply_config_patch(EngineConfig& cfg, std::string_view json_patch) {
  nlohmann::json j = cfg;
  const auto patch = nlohmann::json::parse(json_patch);
  check_known_keys(j, patch, "");
  j.merge_patch(patch);
  cfg = j.get<EngineConfig>();
}

EngineConfig config_from_json(std::string_view json_patch) {
  EngineConfig cfg;
  apply_config_patch(cfg, json_patch);
  return cfg;
}

std::string config_to_json(const EngineConfig& cfg, int indent) { return nlohmann::json(cfg).dump(indent); }

}  // namespace ipe
