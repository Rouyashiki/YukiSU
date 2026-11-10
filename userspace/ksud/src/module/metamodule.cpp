#include "metamodule.hpp"
#include "../../kagami/include/kagami/embedded.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "module.hpp"

#include <sys/stat.h>
#include <filesystem>

namespace ksud {

namespace {
bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::string get_metamodule_module_path(std::string* module_id_out = nullptr) {
    std::string module_path = get_metamodule_path();
    if (module_path.empty()) {
        return "";
    }

    if (module_id_out != nullptr) {
        *module_id_out = std::filesystem::path(module_path).filename().string();
    }
    return module_path;
}

std::string get_enabled_metamodule_script_path(const std::string& script_name,
                                               std::string* module_id_out = nullptr) {
    const std::string module_path = get_metamodule_module_path(module_id_out);
    if (module_path.empty()) {
        return "";
    }

    if (file_exists(module_path + "/" + DISABLE_FILE_NAME)) {
        LOGI("Metamodule is disabled, skipping %s", script_name.c_str());
        return "";
    }

    std::string script_path = module_path + "/" + script_name;
    if (!file_exists(script_path)) {
        return "";
    }

    return script_path;
}
}  // namespace

int metamodule_init() {
    LOGD("Metamodule init");
    return 0;
}

int metamodule_exec_stage_script(const std::string& stage, ScriptWait wait, std::string* error) {
    std::string module_id;
    const std::string script = get_enabled_metamodule_script_path(stage + ".sh", &module_id);
    return run_script(script, wait, module_id, nullptr, nullptr, false,
                      ScriptExitPolicy::ObserveExit, error);
}

int metamodule_exec_mount_script(std::string* error) {
    std::string module_id;
    const std::string script =
        get_enabled_metamodule_script_path(METAMODULE_MOUNT_SCRIPT, &module_id);

    if (!file_exists(script)) {
        LOGI("Using built-in Kagami mount backend");
        const int ret = kagami::embedded_mount();
        if (ret != 0)
            LOGE("Built-in Kagami mount failed: %d", ret);
        return ret;
    }

    LOGI("External metamodule found, executing metamount.sh: %s", script.c_str());
    kagami::embedded_mount_skipped("external metamodule " + module_id);
    const int ret =
        run_script(script, ScriptWait::forever(), module_id, "MODULE_DIR", MODULE_DIR, false,
                   ScriptExitPolicy::RequireSuccess, error, ScriptContext::Inherit);

    if (ret == 0) {
        LOGI("External metamodule mount script executed successfully");
    } else {
        LOGE("External metamodule mount script failed with status: %d", ret);
    }

    return ret;
}

std::string metamodule_mount_owner() {
    std::string module_id;
    const auto script = get_enabled_metamodule_script_path(METAMODULE_MOUNT_SCRIPT, &module_id);
    return script.empty() ? std::string{} : module_id;
}

int metamodule_exec_uninstall_script(const std::string& module_id) {
    std::string metamodule_id;
    const std::string script =
        get_enabled_metamodule_script_path(METAMODULE_METAUNINSTALL_SCRIPT, &metamodule_id);
    return run_script(script, ScriptWait::forever(), metamodule_id, "MODULE_ID", module_id.c_str(),
                      false, ScriptExitPolicy::RequireSuccess, nullptr,
                      ScriptContext::InheritAtScriptDir);
}

}  // namespace ksud
