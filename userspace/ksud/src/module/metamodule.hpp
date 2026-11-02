#pragma once

#include <string>

namespace ksud {

struct ScriptWait;

// Metamodule support
int metamodule_init();
int metamodule_exec_stage_script(const std::string& stage, ScriptWait wait,
                                 std::string* error = nullptr);
int metamodule_exec_mount_script(std::string* error = nullptr);
std::string metamodule_mount_owner();
int metamodule_exec_uninstall_script(const std::string& module_id);

}  // namespace ksud
