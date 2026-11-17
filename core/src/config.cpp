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

IPE_CONFIG_JSON(EngineConfig::Pdr, weinberg_k)
IPE_CONFIG_JSON(EngineConfig::Rssi, sigma_db, wall_aware)
IPE_CONFIG_JSON(EngineConfig::Filter, cell_m, active_floor, estimate_rssi_offset, beacon_integrity, floor_prior_weight,
                fingerprint_weight)
IPE_CONFIG_JSON(EngineConfig::Smoothing, enabled, tau_s, max_correction_mps, snap_m)
IPE_CONFIG_JSON(EngineConfig::Rooms, enter_prob, exit_prob)
IPE_CONFIG_JSON(EngineConfig, pdr, rssi, filter, smoothing, rooms)

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
  if (json_patch.find_first_not_of(" \t\r\n") == std::string_view::npos) return;
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
