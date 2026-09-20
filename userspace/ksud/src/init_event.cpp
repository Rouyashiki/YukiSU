#include "init_event.hpp"
#include "../kagami/include/kagami/embedded.hpp"
#include "assets.hpp"
#include "core/feature.hpp"
#include "core/hide_bootloader.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "core/uts_view.hpp"
#include "defs.hpp"
#include "dynamic_manager.hpp"
#include "integrity_monitor.hpp"
#include "log.hpp"
#include "magisk_compat/msud.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "plugin/lua_engine.hpp"
#include "profile/profile.hpp"
#include "sulog.hpp"
#include "umount.hpp"
#include "utils.hpp"
#include "yukizygisk_diagnostics.hpp"
#include "yukizygisk_snapshot.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ksud {

namespace {

#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
extern "C" int resetprop_main(int argc, char** argv);
#endif

enum class DaemonizeResult : std::uint8_t {
    Parent,
    Daemon,
    Error,
};

DaemonizeResult daemonize_soft_reboot() {
    const pid_t pid = fork();
    if (pid < 0) {
        LOGE("Failed to fork soft reboot daemon: %s", strerror(errno));
        return DaemonizeResult::Error;
    }

    if (pid > 0) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            LOGE("Soft reboot daemon failed to initialize");
            return DaemonizeResult::Error;
        }
        return DaemonizeResult::Parent;
    }

    detach_process_group(true);
    switch_cgroups();
    if (!switch_mnt_ns(1)) {
        LOGE("Failed to enter init mount namespace for soft reboot");
        _exit(1);
    }
    if (chdir("/") != 0) {
        LOGE("Failed to change directory for soft reboot: %s", strerror(errno));
        _exit(1);
    }

    if (!reset_stdio_to_devnull())
        _exit(1);

    const pid_t daemon_pid = fork();
    if (daemon_pid < 0)
        _exit(1);
    if (daemon_pid > 0)
        _exit(0);

    return DaemonizeResult::Daemon;
}

bool reset_boot_completed() {
    LOGI("Resetting sys.boot_completed to 0");
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 4> argv = {
        const_cast<char*>("resetprop"),
        const_cast<char*>("sys.boot_completed"),
        const_cast<char*>("0"),
        nullptr,
    };
    return resetprop_main(3, argv.data()) == 0;
#else
    return exec_command({RESETPROP_PATH, "sys.boot_completed", "0"}).exit_code == 0;
#endif
}

bool wait_for_boot_completed() {
    LOGI("Waiting for sys.boot_completed to change from 0");
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 5> argv = {
        const_cast<char*>("resetprop"),
        const_cast<char*>("-w"),
        const_cast<char*>("sys.boot_completed"),
        const_cast<char*>("0"),
        nullptr,
    };
    return resetprop_main(4, argv.data()) == 0;
#else
    return exec_command({RESETPROP_PATH, "-w", "sys.boot_completed", "0"}).exit_code == 0;
#endif
}

bool run_soft_reboot_command(const char* command) {
    const auto result = exec_command({command});
    if (result.exit_code == 0)
        return true;

    LOGW("%s failed with exit code %d: %s", command, result.exit_code, result.stderr_str.c_str());
    return false;
}

// Catch boot logs (logcat/dmesg) to file
void catch_bootlog(const char* logname, const std::vector<const char*>& command) {
    ensure_dir_exists(LOG_DIR);

    const std::string bootlog = std::string(LOG_DIR) + "/" + logname + ".log";
    const std::string oldbootlog = std::string(LOG_DIR) + "/" + logname + ".old.log";

    // Rotate old log
    if (access(bootlog.c_str(), F_OK) == 0) {
        (void)rename(bootlog.c_str(), oldbootlog.c_str());
    }

    // Fork and exec timeout command
    const pid_t pid = fork();
    if (pid < 0) {
        LOGW("Failed to fork for %s: %s", logname, strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child process
        // Create new process group
        setpgid(0, 0);

        // Switch cgroups
        switch_cgroups();

        // Open log file for stdout
        const int fd = open(bootlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);

        // Build argv: timeout -s 9 30s <command...>
        std::vector<const char*> argv;
        argv.push_back("timeout");
        argv.push_back("-s");
        argv.push_back("9");
        argv.push_back("30s");
        for (const char* arg : command) {
            argv.push_back(arg);
        }
        argv.push_back(nullptr);

        execvp("timeout", const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent: don't wait, let it run in background
    LOGI("Started %s capture (pid %d)", logname, pid);
}

void run_stage(const std::string& stage, bool block) {
    umask(0);

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return;
    }

    // Execute common scripts first
    exec_common_scripts(stage + ".d", block);

    // Execute metamodule stage script (priority)
    metamodule_exec_stage_script(stage, block);

    // Execute regular modules stage scripts
    exec_stage_script(stage, block);

    // Execute plugin stage callbacks
    exec_plugin_stage(stage, block);
}

int spawn_zygiskd_process(const char* path, const char* label) {
    int ready_pipe[2] = {-1, -1};
    if (pipe(ready_pipe) != 0) {
        LOGE("Failed to create %s ready pipe: %s", label, strerror(errno));
        return -1;
    }

    pid_t const pid = fork();
    if (pid < 0) {
        LOGE("Failed to fork %s launcher: %s", label, strerror(errno));
        if (ready_pipe[0] >= 0)
            close(ready_pipe[0]);
        if (ready_pipe[1] >= 0)
            close(ready_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        if (ready_pipe[0] >= 0)
            close(ready_pipe[0]);

        if (setpgid(0, 0) != 0) {
            LOGW("Failed to detach %s process group: %s", label, strerror(errno));
        }
        switch_cgroups();

        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const pid_t grandchild = fork();
        if (grandchild < 0) {
            if (ready_pipe[1] >= 0) {
                const char fail = '0';
                write(ready_pipe[1], &fail, 1);
                close(ready_pipe[1]);
            }
            _exit(127);
        }
        if (grandchild > 0) {
            if (ready_pipe[1] >= 0)
                close(ready_pipe[1]);
            _exit(0);
        }

        if (ready_pipe[1] >= 0) {
            char fd_env[16];
            (void)snprintf(fd_env, sizeof(fd_env), "%d", ready_pipe[1]);
            setenv("YUKIZYGISK_READY_FD", fd_env, 1);
        }

        char* const argv[] = {const_cast<char*>(path), nullptr};
        execv(path, argv);

        if (ready_pipe[1] >= 0) {
            const char fail = '0';
            write(ready_pipe[1], &fail, 1);
            close(ready_pipe[1]);
        }
        _exit(127);
    }

    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);

    pollfd pfd{};
    pfd.fd = ready_pipe[0];
    pfd.events = POLLIN | POLLHUP;
    int pr;
    do {
        pr = poll(&pfd, 1, 5000);
    } while (pr < 0 && errno == EINTR);

    char ready = '0';
    ssize_t received = -1;
    if (pr > 0 && (pfd.revents & POLLIN)) {
        do {
            received = read(ready_pipe[0], &ready, 1);
        } while (received < 0 && errno == EINTR);
    }
    close(ready_pipe[0]);

    const bool daemon_ready = received == 1 && ready == '1';
    if (!daemon_ready) {
        LOGE("%s failed readiness (poll=%d, revents=0x%x, byte=%c)", label, pr, pfd.revents, ready);
        (void)kill(-pid, SIGKILL);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        LOGW("waitpid for %s launcher failed: %s", label, strerror(errno));
    }

    if (daemon_ready) {
        LOGI("%s reported ready", label);
        return 0;
    }
    return -1;
}

bool has_zygote32() {
    if (access("/system_ext/bin/tango_translator", X_OK) == 0 &&
        access("/system/bin/app_process32", X_OK) == 0) {
        return true;
    }
    const auto zygote = getprop("ro.zygote");
    if (zygote) {
        return zygote->find("32") != std::string::npos;
    }
    return access("/system/bin/app_process32", X_OK) == 0;
}

bool needs_zygiskd32() {
    return has_zygote32() || yukizygisk_has_native_abi32_target();
}

// Launch the daemons required by configured zygotes and native targets.
int spawn_zygiskd() {
    int result = 0;
    struct Daemon {
        const char* path;
        const char* label;
        bool enabled;
    };
    const Daemon daemons[] = {
        {ZYGISKD64_PATH, "zygiskd64", true},
        {ZYGISKD32_PATH, "zygiskd32", needs_zygiskd32()},
    };

    for (const auto& daemon : daemons) {
        if (!daemon.enabled) {
            LOGI("No 32-bit zygote or native target configured; skipping %s", daemon.label);
            continue;
        }
        if (access(daemon.path, X_OK) != 0) {
            LOGW("%s is unavailable at %s", daemon.label, daemon.path);
            result = -1;
            continue;
        }
        if (spawn_zygiskd_process(daemon.path, daemon.label) != 0) {
            result = -1;
        }
    }
    return result;
}

bool yukizygisk_feature_enabled() {
    if (is_safe_mode()) {
        return false;
    }
    const auto [value, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    return supported && value != 0;
}

void ensure_yukizygisk_payload_if_enabled() {
    if (!yukizygisk_feature_enabled()) {
        return;
    }
    ensure_yukizygisk(true);
}

void ensure_zygiskd_running_if_enabled() {
    if (!yukizygisk_feature_enabled())
        return;
    LOGI("YukiZygisk feature on -- launching zygiskd");
    if (spawn_zygiskd() != 0) {
        LOGE("One or more YukiZygisk daemons failed to start");
    }
}

}  // namespace

int on_post_data_fs() {
    LOGI("post-fs-data triggered");
    kagami::embedded_post_fs_data();
    (void)prepare_yukizygisk_diagnostics(false);

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip post-fs-data due to UAPI version mismatch");
        kagami::embedded_mount_skipped("UAPI version mismatch");
        return 0;
    }

    if (set_init_pgrp() != 0) {
        LOGW("set init pgrp failed");
    }

    // Report to kernel first
    report_post_fs_data();
    load_and_apply_dynamic_managers();

    if (!start_ksud_integrity_monitor()) {
        LOGW("Failed to start ksud integrity monitor");
    }

    umask(0);

    // Clear all temporary module configs early (like Rust version)
    clear_all_temp_configs();

    // Catch boot logs
    catch_bootlog("logcat", {"logcat", "-b", "all"});
    catch_bootlog("dmesg", {"dmesg", "-w"});

    const bool safe_mode = is_safe_mode();
    const auto [early_yz_value, early_yz_supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    update_yukizygisk_boot_diagnostics(safe_mode, early_yz_supported,
                                       !safe_mode && early_yz_supported && early_yz_value != 0,
                                       "pre-restore");

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip post-fs-data!");
        kagami::embedded_mount_skipped("Magisk owns boot handling");
        return 0;
    }

    if (safe_mode) {
        LOGW("safe mode, skip common post-fs-data.d scripts");
    } else {
        // Execute common post-fs-data scripts
        exec_common_scripts("post-fs-data.d", true);
    }

    // Ensure directories exist
    ensure_dir_exists(WORKING_DIR);
    ensure_dir_exists(MODULE_DIR);
    ensure_dir_exists(LOG_DIR);
    ensure_dir_exists(PROFILE_DIR);

    // Ensure binaries exist (AFTER safe mode check, like Rust)
    if (ensure_binaries(true) != 0) {
        LOGW("Failed to ensure binaries");
    }

    if (!restorecon())
        LOGW("Failed to migrate KernelSU file contexts");

    // if we are in safe mode, we should disable all modules
    if (safe_mode) {
        LOGW("safe mode, skip post-fs-data scripts and disable all modules!");
        kagami::embedded_mount_skipped("safe mode");
        disable_all_modules();
        return 0;
    }

    // Handle updated modules
    handle_updated_modules();

    // Prune modules marked for removal
    prune_modules();

    // Refresh custom init rc for the next boot. This also covers manual edits in
    // /data/adb/initrc.d.
    if (regenerate_preinit_rc() != 0) {
        LOGW("regenerate preinit rc failed");
    }

    // Restorecon
    restorecon("/data/adb", true);

    // Load sepolicy rules from modules
    load_sepolicy_rule();

    // Apply profile sepolicies
    apply_profile_sepolies();

    // Restore the independent UTS extension before generic feature handling.
    if (apply_uts_view_config() != 0) {
        LOGW("apply persisted UTS View configuration failed");
    }

    // Load feature config (with init_features handling managed features)
    init_features();
    const auto [yz_value, yz_supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    const bool yz_enabled = yz_supported && yz_value != 0;
    if (yz_enabled)
        (void)prepare_yukizygisk_diagnostics(true);
    update_yukizygisk_boot_diagnostics(false, yz_supported, yz_enabled, "restored");
    ensure_yukizygisk_payload_if_enabled();
    if (refresh_yukizygisk_early_snapshot() != 0) {
        LOGW("refresh YukiZygisk early snapshot failed");
    }
    ensure_sulogd_running_if_enabled();
    ensure_zygiskd_running_if_enabled();

    // KernelSU execution order (https://kernelsu.org/guide/metamodule.html):
    // 1. Common post-fs-data.d, prune, restorecon, sepolicy
    // 2. Metamodule's post-fs-data.sh
    // 3. Regular modules' post-fs-data.sh
    // 4. Load system.prop
    // 5. Metamodule's metamount.sh  <-- MUST run AFTER all post-fs-data
    // 6. post-mount.d

    metamodule_exec_stage_script("post-fs-data", true);
    exec_stage_script("post-fs-data", true);
    exec_plugin_stage("post-fs-data", true);
    load_system_prop();

    // Metamodule metamount runs AFTER all post-fs-data.
    metamodule_exec_mount_script();

    umount_apply_config();

    run_stage("post-mount", true);
    if (refresh_sucompat_vfs() != 0) {
        LOGW("refresh vnode-backed su after final mounts failed");
    }

    chdir("/");

    LOGI("post-fs-data completed");
    return 0;
}

void on_services() {
    LOGI("services triggered");

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip services due to UAPI version mismatch");
        return;
    }

    // Hide bootloader unlock status (soft BL hiding)
    // Service stage is the correct timing - after boot_completed is set
    hide_bootloader_status();

    run_stage("service", false);

    LOGI("services completed");
}

void on_boot_completed() {
    LOGI("boot-completed triggered");

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip boot-completed due to UAPI version mismatch");
        return;
    }

    // Report to kernel
    report_boot_complete();
    kagami::embedded_boot_completed();

    ensure_msud_running_if_enabled();

    // Run boot-completed stage
    run_stage("boot-completed", false);

    LOGI("boot-completed completed");
}

int soft_reboot() {
    std::string version_error;
    if (!ensure_uapi_version_matched(&version_error)) {
        LOGE("Skip soft reboot due to UAPI version mismatch: %s", version_error.c_str());
        return 0;
    }

    switch (daemonize_soft_reboot()) {
    case DaemonizeResult::Parent:
        return 0;
    case DaemonizeResult::Error:
        return 1;
    case DaemonizeResult::Daemon:
        break;
    }

    LOGI("Emulating soft reboot");
    if (!reset_boot_completed())
        LOGW("Failed to reset sys.boot_completed");

    run_stage("emulated-soft-reboot", true);

    LOGI("Stopping Android services");
    (void)run_soft_reboot_command("stop");

    LOGI("Running post-fs-data after stop");
    (void)on_post_data_fs();

    LOGI("Starting Android services");
    (void)run_soft_reboot_command("start");

    on_services();
    if (!wait_for_boot_completed())
        LOGW("Failed while waiting for boot completion");
    on_boot_completed();

    _exit(0);
}

}  // namespace ksud
