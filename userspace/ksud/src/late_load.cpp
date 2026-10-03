#include "late_load.hpp"

#include "assets.hpp"
#include "boot/boot_patch.hpp"
#include "core/feature.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "core/uts_view.hpp"
#include "defs.hpp"
#include "init_event.hpp"
#include "kernelsu_loader.hpp"
#include "log.hpp"
#include "magica/magica.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "profile/profile.hpp"
#include "umount.hpp"
#include "utils.hpp"
#include "yukizygisk_diagnostics.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ksud::late_load {

namespace {

constexpr const char* kManagerPackage = "com.anatdx.yukisu";
constexpr const char* kLateLoadTmpCandidates[] = {
    "/dev/kernelsu_XXXXXX",
    "/data/local/tmp/kernelsu_XXXXXX",
    "/data/adb/kernelsu_XXXXXX",
};

bool is_kernelsu_loaded() {
    if (access("/sys/module/kernelsu", F_OK) == 0)
        return true;

    // Release builds remove their sysfs kobject after initialization, so
    // /sys/module alone is not a reliable one-shot guard. /proc/modules keeps
    // the exact live module name without touching the KernelSU fd cache.
    const auto modules = read_file("/proc/modules");
    if (!modules)
        return false;
    bool loaded = false;
    for_each_line(*modules, [&loaded](std::string_view line) {
        if (line.rfind("kernelsu ", 0) == 0)
            loaded = true;
    });
    return loaded;
}

std::string get_kernelsu_load_params(bool allow_shell) {
    if (allow_shell || access("/ksu_allow_shell", F_OK) == 0) {
        LOGW("late-load: loading kernelsu.ko with allow_shell=1");
        return "allow_shell=1";
    }

    return "";
}

bool extract_and_load_kernelsu(bool allow_shell) {
    // The driver and its UTS extension do not exist yet. Probing the private
    // status ioctl here would both fail closed and poison the process-wide
    // driver-fd cache before the module can be loaded.
    const std::string kmi = get_bootstrap_kmi();
    if (kmi.empty()) {
        LOGE("late-load: failed to detect current KMI");
        return false;
    }

    const std::string asset_name = kmi + "_kernelsu.ko";
    std::array<char, PATH_MAX> tmp_path{};
    int tmp_fd = -1;
    for (const char* candidate : kLateLoadTmpCandidates) {
        (void)std::snprintf(tmp_path.data(), tmp_path.size(), "%s", candidate);
        tmp_fd = mkstemp(tmp_path.data());
        if (tmp_fd >= 0) {
            LOGI("late-load: using temp module path %s", tmp_path.data());
            break;
        }
        LOGW("late-load: mkstemp failed at %s: %s", candidate, strerror(errno));
    }
    if (tmp_fd < 0) {
        LOGE("late-load: failed to allocate temp module file");
        return false;
    }
    close(tmp_fd);

    const bool copied = copy_asset_to_file(asset_name, tmp_path.data());
    if (!copied) {
        LOGE("late-load: no embedded module matches %s", asset_name.c_str());
        unlink(tmp_path.data());
        return false;
    }

    LOGI("late-load: loading %s via relocated init_module", asset_name.c_str());
    const bool loaded =
        ksud::kernelsu_loader::load_module(tmp_path.data(), get_kernelsu_load_params(allow_shell));
    unlink(tmp_path.data());
    return loaded;
}

void run_stage_scripts(const std::string& stage, ScriptWait wait) {
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    exec_common_scripts(stage + ".d", wait);
    metamodule_exec_stage_script(stage, wait);
    exec_stage_script(stage, wait);
}

void restart_manager() {
    const std::string activity = std::string(kManagerPackage) + "/.ui.MainActivity";
    (void)exec_command({"am", "force-stop", kManagerPackage});
    (void)exec_command({"am", "start", "-n", activity});
}

bool run_finalizer_command(const std::vector<std::string>& args, const char* prefix) {
    const auto result = exec_command(args);
    if (result.exit_code == 0) {
        return true;
    }

    LOGE("%s failed: %s", prefix, args.front().c_str());
    if (!result.stdout_str.empty()) {
        LOGE("stdout: %s", result.stdout_str.c_str());
    }
    if (!result.stderr_str.empty()) {
        LOGE("stderr: %s", result.stderr_str.c_str());
    }
    return false;
}

void run_post_magica_cleanup() {
    LOGI("Running post-magica finalization after late-load");

    if (!run_finalizer_command({"setenforce", "1"}, "late-load post-magica")) {
        LOGE("Failed to re-enable SELinux enforcing after Magica late-load");
    }

    if (magica::disable_adb_root() != 0) {
        LOGE("Failed to restore adb properties after Magica late-load");
    }
}

}  // namespace

int run(bool post_magica, bool allow_shell) {
    switch (daemonize_process(false)) {
    case ProcessDaemonizeResult::Parent:
        return 0;
    case ProcessDaemonizeResult::Error:
        return 1;
    case ProcessDaemonizeResult::Daemon:
        break;
    }

    LOGI("late-load command triggered");
    const auto execute = [allow_shell]() -> int {
        if (!is_kernelsu_loaded() && !extract_and_load_kernelsu(allow_shell)) {
            return 1;
        }

        // Loading the LKM may transition this process into the KernelSU domain.
        // Reopen standard descriptors so later Binder fd transfers carry the
        // current SELinux identity rather than that of the bootstrap shell.
        if (!reset_stdio_to_devnull()) {
            LOGE("late-load: failed to reset stdio after loading the LKM");
            return 1;
        }

        (void)prepare_yukizygisk_diagnostics(false);

        ksud::umask(0);

        clear_all_temp_configs();

        if (install(std::nullopt, std::nullopt) != 0) {
            LOGE("late-load: install() failed");
            return 1;
        }

        if (handle_updated_modules() != 0) {
            LOGW("late-load: handle_updated_modules failed");
        }

        if (prune_modules() != 0) {
            LOGW("late-load: prune_modules failed");
        }

        if (!restorecon()) {
            LOGW("late-load: restorecon failed");
        }

        if (load_sepolicy_rule() != 0) {
            LOGW("late-load: load_sepolicy_rule failed");
        }

        if (apply_profile_sepolies() != 0) {
            LOGW("late-load: apply_profile_sepolies failed");
        }

        if (apply_uts_view_config() != 0) {
            LOGW("late-load: apply persisted UTS View configuration failed");
        }

        if (init_features() != 0) {
            LOGW("late-load: init_features failed");
        }
        const bool safe_mode = is_safe_mode();
        const auto [yz_value, yz_supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
        const bool yz_enabled = !safe_mode && yz_supported && yz_value != 0;
        if (yz_enabled)
            (void)prepare_yukizygisk_diagnostics(true);
        update_yukizygisk_boot_diagnostics(safe_mode, yz_supported, yz_enabled,
                                           "late-load-restored");

        const auto wait = ScriptWait::until(std::chrono::steady_clock::now() + BOOT_STAGE_TIMEOUT);
        run_stage_scripts("late-load", wait);

        if (load_system_prop() != 0) {
            LOGW("late-load: load_system_prop failed");
        }

        if (metamodule_exec_mount_script() != 0) {
            LOGW("late-load: metamodule_exec_mount_script failed");
        }

        if (umount_apply_config() != 0) {
            LOGW("late-load: umount_apply_config failed");
        }

        run_stage_scripts("post-mount", wait);
        if (refresh_sucompat_vfs() != 0) {
            LOGW("late-load: refresh vnode-backed su after final mounts failed");
        }
        on_services();
        on_boot_completed();

        restart_manager();

        return 0;
    };

    const int result = execute();
    if (post_magica) {
        run_post_magica_cleanup();
    }

    return result;
}

}  // namespace ksud::late_load
