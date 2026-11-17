#pragma once

#include <string>
#include <string_view>

#include "ipe/engine.hpp"

namespace ipe {

/// The engine's settings as JSON. recommended_config(venue) in engine.hpp gives the settings a venue calls
/// for; a patch changes single values of them (`--set`, `--config`).

/// Applies an RFC 7386 merge patch to `cfg`, e.g. `{"filter": {"cell_m": 0.25}}`. Unknown keys throw
/// std::invalid_argument, so a typo fails loudly instead of silently leaving a value in place. An empty
/// patch changes nothing.
void apply_config_patch(EngineConfig& cfg, std::string_view json_patch);

/// The default config with `json_patch` applied.
[[nodiscard]] EngineConfig config_from_json(std::string_view json_patch);

/// Every setting with its value, in the form apply_config_patch reads.
[[nodiscard]] std::string config_to_json(const EngineConfig& cfg, int indent = 2);

}  // namespace ipe
