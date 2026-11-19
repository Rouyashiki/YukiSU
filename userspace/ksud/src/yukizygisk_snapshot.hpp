#pragma once

#include "core/json.hpp"

namespace ksud {

int refresh_yukizygisk_early_snapshot();
bool prepare_yukizygisk_policy_dirs();
bool save_yukizygisk_policy_boot();
bool stage_yukizygisk_policy_boot(const json::Value& config);
void clear_yukizygisk_early_snapshot();
bool yukizygisk_early_load_enabled();
bool yukizygisk_has_native_abi32_target();

}  // namespace ksud
