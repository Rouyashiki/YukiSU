#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace ksud {

struct CommonScriptEnv {
    std::string kernel_ver_code;
    std::string uapi_version;
    std::string runtime_mode;
    std::string path;
    bool late_load{};
    bool zygisk_enabled{};
};

// Module management
int module_install(const std::string& zip_path);
int module_uninstall(const std::string& id);
int module_undo_uninstall(const std::string& id);
int module_enable(const std::string& id);
int module_disable(const std::string& id);
int module_run_action(const std::string& id);
// Reports each module's runtime kind and load state. When the built-in YukiZygisk
// feature is on, it also force-disables the conflicting Zygisk implementations
// before the scan, so the listing can never hand out a stale enabled state.
int module_list();

// Internal functions
int uninstall_all_modules();
int prune_modules();
int disable_all_modules();
int handle_updated_modules();
int regenerate_preinit_rc();

// Script execution
struct ScriptWait {
    enum class Mode { NoWait, Forever, Until };

    Mode mode;
    std::chrono::steady_clock::time_point deadline{};

    static ScriptWait no_wait() { return {Mode::NoWait}; }
    static ScriptWait forever() { return {Mode::Forever}; }
    static ScriptWait until(std::chrono::steady_clock::time_point deadline) {
        return {Mode::Until, deadline};
    }
};

int run_script(const std::string& script, ScriptWait wait, const std::string& module_id = "",
               const char* extra_env_name = nullptr, const char* extra_env_value = nullptr);
int exec_stage_script(const std::string& stage, ScriptWait wait);
int exec_common_scripts(const std::string& stage_dir, ScriptWait wait);
int load_sepolicy_rule();
int load_system_prop();

// Get all managed features from active modules
// Modules declare managed features via config system (manage.<feature>=true)
// Returns: map<ModuleId, vector<ManagedFeature>>
std::map<std::string, std::vector<std::string>> get_managed_features();

// Metamodule
std::string get_metamodule_id();

// Shared script environment
CommonScriptEnv build_common_script_env();
void apply_common_script_env(const CommonScriptEnv& env, const char* module_id = nullptr,
                             bool set_magisk_compat = false);

}  // namespace ksud
