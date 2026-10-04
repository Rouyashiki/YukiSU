#pragma once

#include <string>
#include <vector>

namespace ksud {

constexpr const char* SU_PATH_CONFIG_PATH = "/data/adb/ksu/su_path";

// The caller holds SucompatTransitionLock during feature restoration.
int restore_su_path(bool* custom = nullptr);
int reset_su_path_locked();
int su_path_command(const std::vector<std::string>& args);

}  // namespace ksud
