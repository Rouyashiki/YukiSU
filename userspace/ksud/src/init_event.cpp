#include "init_event.hpp"
#include "../kagami/include/kagami/embedded.hpp"
#include "../kagami/include/kagami/kasumi_client.hpp"
#include "../kagami/src/core/daemon.hpp"
#include "../kagami/src/core/reboot_state.hpp"
#include "../kagami/src/core/runtime.hpp"
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
#include "module/hot_install.hpp"
#include "module/metamodule.hpp"
#include "module/module.hpp"
#include "module/module_config.hpp"
#include "module/preinit.hpp"
#include "plugin/lua_engine.hpp"
#include "profile/profile.hpp"
#include "soft_reboot_flow.hpp"
#include "soft_reboot_waiter.hpp"
#include "sulog.hpp"
#include "umount.hpp"
#include "utils.hpp"
#include "yukizygisk_daemon.hpp"
#include "yukizygisk_diagnostics.hpp"
#include "yukizygisk_snapshot.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
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
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

DaemonizeResult daemonize_soft_reboot(int& ready_fd) {
    int channel[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) != 0)
        return DaemonizeResult::Error;
    const pid_t pid = fork();
    if (pid < 0) {
        close(channel[0]);
        close(channel[1]);
        LOGE("Failed to fork soft reboot daemon: %s", strerror(errno));
        return DaemonizeResult::Error;
    }

    if (pid > 0) {
        close(channel[1]);
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            close(channel[0]);
            LOGE("Soft reboot daemon failed to initialize");
            return DaemonizeResult::Error;
        }
        pollfd descriptor{channel[0], POLLIN, 0};
        char ready = 0;
        const bool accepted = poll(&descriptor, 1, 30000) > 0 &&
                              recv(channel[0], &ready, 1, 0) == 1 && ready == 1 &&
                              send(channel[0], &ready, 1, MSG_NOSIGNAL) == 1;
        close(channel[0]);
        return accepted ? DaemonizeResult::Parent : DaemonizeResult::Error;
    }

    close(channel[0]);
    ready_fd = channel[1];

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

bool set_boot_completed(const char* value) {
    LOGI("Setting sys.boot_completed to %s", value);
#if defined(RESETPROP_ALONE_AVAILABLE) && RESETPROP_ALONE_AVAILABLE
    std::array<char*, 4> argv = {
        const_cast<char*>("resetprop"),
        const_cast<char*>("sys.boot_completed"),
        const_cast<char*>(value),
        nullptr,
    };
    return resetprop_main(3, argv.data()) == 0;
#else
    return exec_command({RESETPROP_PATH, "sys.boot_completed", value}, std::chrono::seconds(5))
               .exit_code == 0;
#endif
}

bool wait_for_boot_completed() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    do {
        if (getprop("sys.boot_completed") == "1")
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool run_soft_reboot_command(const char* command) {
    const auto result = exec_command({command}, std::chrono::seconds(10));
    if (result.exit_code == 0)
        return true;

    LOGW("%s failed with exit code %d: %s", command, result.exit_code, result.stderr_str.c_str());
    return false;
}

bool capture_soft_reboot_log(std::string& error) {
    const auto path = kagami::runtime_data_dir() / "run" / "soft-reboot.log";
    const auto previous = path.string() + ".old";
    if (!kagami::prepare_private_file(path, error))
        return false;
    if (rename(path.c_str(), previous.c_str()) != 0 && errno != ENOENT) {
        error = "cannot rotate soft reboot log: " + std::string(strerror(errno));
        return false;
    }
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "cannot open soft reboot log: " + std::string(strerror(errno));
        return false;
    }
    const bool redirected = dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0;
    const int redirect_error = errno;
    close(fd);
    if (!redirected) {
        error = "cannot redirect soft reboot log: " + std::string(strerror(redirect_error));
        return false;
    }
    log_set_stderr_enabled(true);
    return true;
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

int run_stage(const std::string& stage, ScriptWait wait, std::string* error = nullptr) {
    umask(0);

    // Check for Magisk (like Rust version)
    if (has_magisk()) {
        LOGW("Magisk detected, skip %s", stage.c_str());
        return 0;
    }

    if (is_safe_mode()) {
        LOGW("safe mode, skip %s scripts", stage.c_str());
        return 0;
    }

    // Execute common scripts first
    const int common = exec_common_scripts(stage + ".d", wait, error);

    // Execute metamodule stage script (priority)
    const int meta = metamodule_exec_stage_script(stage, wait, error);

    // Execute regular modules stage scripts
    const int modules = exec_stage_script(stage, wait, error);

    // Execute plugin stage callbacks
    const bool plugins = exec_plugin_stage(stage, wait.mode != ScriptWait::Mode::NoWait);
    if (!plugins && error && error->empty())
        *error = "plugin callback failed during " + stage;
    return common != 0 || meta != 0 || modules != 0 || !plugins ? 1 : 0;
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
    if (ensure_yukizygisk_daemons() != 0) {
        LOGE("One or more YukiZygisk daemons failed to start");
    }
}

}  // namespace

int on_post_data_fs(std::string* error) {
    LOGI("post-fs-data triggered");
    const auto fail = [&](const char* step) {
        if (error && error->empty())
            *error = std::string("post-fs-data: ") + step;
        LOGE("post-fs-data failed: %s", step);
        return 1;
    };
    const bool restoring = kagami::reboot::authorized();
    if (kagami::reboot::active() && !restoring)
        return fail("another soft reboot owns the controller");
    kagami::embedded_post_fs_data();
    (void)prepare_yukizygisk_diagnostics(false);

    if (!ensure_uapi_version_matched(error)) {
        LOGE("Skip post-fs-data due to UAPI version mismatch");
        kagami::embedded_mount_skipped("UAPI version mismatch");
        return restoring ? fail("UAPI version mismatch") : 0;
    }

    if (set_init_pgrp() != 0) {
        LOGW("set init pgrp failed");
    }

    // Report to kernel first
    report_post_fs_data();
    init_forced_features();
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

    const auto wait = ScriptWait::until(std::chrono::steady_clock::now() + BOOT_STAGE_TIMEOUT);
    // Ensure directories exist
    ensure_dir_exists(WORKING_DIR);
    ensure_dir_exists(MODULE_DIR);
    ensure_dir_exists(LOG_DIR);
    ensure_dir_exists(PROFILE_DIR);

    // Ensure binaries exist (AFTER safe mode check, like Rust)
    if (ensure_binaries(true) != 0) {
        LOGW("Failed to ensure binaries");
        if (restoring)
            return fail("ensure binaries");
    }

    clean_hot_install_trash();

    if (safe_mode) {
        LOGW("safe mode, skip common post-fs-data.d scripts");
    } else {
        // Execute common post-fs-data scripts
        if (exec_common_scripts("post-fs-data.d", wait, error) != 0 && restoring)
            return fail("common scripts");
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
    if (handle_updated_modules() != 0 && restoring)
        return fail("apply module updates");

    // Prune modules marked for removal
    if (prune_modules() != 0 && restoring)
        return fail("remove modules");

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

    const int meta_scripts = metamodule_exec_stage_script("post-fs-data", wait, error);
    const int module_scripts = exec_stage_script("post-fs-data", wait, error);
    const bool plugins = exec_plugin_stage("post-fs-data", true);
    const int properties = load_system_prop();
    if (restoring) {
        if (meta_scripts != 0 || module_scripts != 0)
            return fail("module scripts");
        if (!plugins)
            return fail("plugin callbacks");
        if (properties != 0)
            return fail("load system.prop");
    }

    // Metamodule metamount runs AFTER all post-fs-data.
    const int mount_result = metamodule_exec_mount_script(error);
    if (mount_result != 0 && restoring)
        return fail("mount modules");

    umount_apply_config();

    if (run_stage("post-mount", wait, error) != 0 && restoring)
        return fail("post-mount scripts");
    if (refresh_sucompat_vfs() != 0) {
        LOGW("refresh vnode-backed su after final mounts failed");
    }

    chdir("/");

    LOGI("post-fs-data completed");
    return 0;
}

int on_services(std::string* error) {
    const bool restoring = kagami::reboot::authorized();
    if (kagami::reboot::active() && !restoring)
        return 0;
    if (!ensure_uapi_version_matched(error)) {
        LOGE("Skip services due to UAPI version mismatch");
        return 1;
    }

    if (restoring) {
        std::string claim_error;
        const int claim = kagami::reboot::claim_services(claim_error);
        if (claim <= 0) {
            if (claim < 0 && error)
                *error = claim_error;
            return claim < 0 ? 1 : 0;
        }
    }
    const int start = report_services();
    if (start < 0) {
        if (error)
            *error = "cannot report services: " + std::string(strerror(errno));
        LOGE("Failed to report services: %s", strerror(errno));
        return 1;
    }
    if (start != 1 && !restoring) {
        LOGI("services already started, skipping");
        return 0;
    }

    LOGI("services triggered");

    // Hide bootloader unlock status (soft BL hiding)
    hide_bootloader_status();

    const int result = run_stage("service", ScriptWait::no_wait(), error);

    LOGI("services completed");
    return result;
}

int on_boot_completed() {
    LOGI("boot-completed triggered");

    if (!ensure_uapi_version_matched()) {
        LOGE("Skip boot-completed due to UAPI version mismatch");
        return 1;
    }

    namespace reboot = kagami::reboot;
    if (reboot::active() && !reboot::authorized())
        return 0;
    std::string error;
    if (!kagami::prepare_runtime(error)) {
        LOGE("Cannot prepare boot-completed state: %s", error.c_str());
        return 1;
    }
    const auto marker = kagami::runtime_data_dir() / "run" / "boot_completed_cycle";
    if (reboot::marker_matches_cycle(marker)) {
        std::ifstream saved(marker);
        std::string cycle;
        std::string result;
        std::getline(saved, cycle);
        std::getline(saved, result);
        return result.empty() || result == "complete" ? 0 : 1;
    }
    if (!reboot::write_cycle_marker(marker, "running\n"))
        return 1;

    // Report to kernel
    report_boot_complete();
    preinit::on_boot_completed();
    kagami::embedded_boot_completed();

    ensure_msud_running_if_enabled();

    // Run boot-completed stage
    const int result = run_stage("boot-completed", ScriptWait::no_wait());

    LOGI("boot-completed completed");
    const bool recorded =
        reboot::write_cycle_marker(marker, result == 0 ? "complete\n" : "failed\n");
    return recorded ? result : 1;
}

bool recover_soft_reboot(const std::string& phase, std::string& error) {
    const bool before_stop = phase == "reset_boot" || phase == "scripts";
    if (!(before_stop ? set_boot_completed("1") : run_soft_reboot_command("start"))) {
        error = before_stop ? "cannot restore boot-completed property"
                            : "Android recovery start failed";
        return false;
    }
    return phase == "reset_boot" || on_services(&error) == 0;
}

int soft_reboot_status() {
    using namespace kagami;
    const bool running = reboot::active();
    std::string reason;
    bool supported =
        !running && reboot::check_support(reason) && daemon_supports_soft_reboot(reason);
    if (running)
        reason = "soft reboot already running";
    if (supported && getprop("sys.boot_completed") != "1") {
        supported = false;
        reason = "Android has not completed boot";
    }
    auto last = reboot::state();
    last.o.erase("token");
    if (!running && last.at("boot_id").string_or("") == runtime_boot_id()) {
        const auto result = last.at("result").string_or("");
        if (result == "accepted") {
            last["result"] = "failed";
            last["error"] =
                "soft reboot coordinator exited during " + last.at("phase").string_or("");
        } else if (result == "completed_pending") {
            int pending = 0;
            std::string error;
            const bool restored = embedded_verify_hide_rules(pending, error);
            last["pending"] = pending;
            last["result"] = pending ? "completed_pending" : "completed";
            if (!restored)
                last["result"] = "failed";
            last["error"] = error;
        }
    }
    JsonValue result = JsonValue::object();
    result["interface_version"] = 1;
    result["supported"] = supported;
    result["blocked_reason"] = reason;
    result["running"] = running;
    result["last"] = last;
    std::cout << stringify_json(result) << '\n';
    return 0;
}

int soft_reboot() {
    namespace reboot = kagami::reboot;
    std::string error;
    if (reboot::active() || !reboot::check_support(error) ||
        !kagami::daemon_supports_soft_reboot(error) || getprop("sys.boot_completed") != "1") {
        std::cerr << (error.empty() ? "soft reboot is busy or Android has not completed boot"
                                    : error)
                  << '\n';
        return 1;
    }

    int ready_fd = -1;
    switch (daemonize_soft_reboot(ready_fd)) {
    case DaemonizeResult::Parent:
        return 0;
    case DaemonizeResult::Error:
        std::cerr << "soft reboot was not accepted; inspect soft-reboot-status --json\n";
        return 1;
    case DaemonizeResult::Daemon:
        break;
    }

    const bool accepted = reboot::acquire(error) && capture_soft_reboot_log(error) &&
                          reboot::check_support(error) &&
                          kagami::run_via_daemon({"reboot-cycle", "begin"}) == 0;
    char ready = accepted ? 1 : 0;
    bool acknowledged = send(ready_fd, &ready, 1, MSG_NOSIGNAL) == 1;
    if (accepted && acknowledged) {
        pollfd descriptor{ready_fd, POLLIN, 0};
        acknowledged =
            poll(&descriptor, 1, 5000) > 0 && recv(ready_fd, &ready, 1, 0) == 1 && ready == 1;
    }
    close(ready_fd);
    if (!accepted || !acknowledged) {
        (void)reboot::record("preflight", "failed",
                             error.empty() ? "controller or caller handshake failed" : error);
        reboot::release();
        _exit(1);
    }

    SoftRebootWaiter waiter;
    bool cleanup_confirmed = true;
    int pending = 0;
    const auto controller = [&](const char* action) {
        const int status = kagami::run_via_daemon({"reboot-cycle", action});
        cleanup_confirmed = kagami::last_daemon_request_completed();
        return status == 0;
    };
    SoftRebootActions actions;
    actions.run = [&](SoftRebootStage stage) {
        error.clear();
        const auto* name = soft_reboot_stage_name(stage);
        if (!reboot::record(name, "accepted", {}, pending)) {
            error = "cannot persist soft reboot phase";
            return false;
        }
        LOGI("Soft reboot phase: %s", name);
        switch (stage) {
        case SoftRebootStage::Scripts:
            return run_stage("emulated-soft-reboot",
                             ScriptWait::until(std::chrono::steady_clock::now() +
                                               EMULATED_SOFT_REBOOT_TIMEOUT),
                             &error) == 0;
        case SoftRebootStage::PrepareWaiter:
            return waiter.prepare();
        case SoftRebootStage::ResetBoot:
            return set_boot_completed("0") && getprop("sys.boot_completed") == "0";
        case SoftRebootStage::Stop:
            return run_soft_reboot_command("stop");
        case SoftRebootStage::WaitStopped:
            return waiter.wait();
        case SoftRebootStage::Cleanup:
            return controller("prepare");
        case SoftRebootStage::Rebuild: {
            if (!reboot::check_support(error))
                return false;
            const int result = on_post_data_fs(&error);
            cleanup_confirmed = kagami::last_daemon_request_completed();
            return result == 0;
        }
        case SoftRebootStage::RestoreHide: {
            const bool restored = controller("restore-hide");
            pending = static_cast<int>(reboot::state().at("pending").u32_or(0));
            return restored;
        }
        case SoftRebootStage::Start:
            return run_soft_reboot_command("start");
        case SoftRebootStage::Services:
            return on_services(&error) == 0;
        case SoftRebootStage::WaitBoot:
            return wait_for_boot_completed();
        case SoftRebootStage::Complete:
            return on_boot_completed() == 0;
        case SoftRebootStage::Verify: {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            do {
                error.clear();
                if (kagami::embedded_verify_hide_rules(pending, error))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }
        }
        return false;
    };
    actions.rollback = [&] {
        if (!controller("rollback"))
            error += "; partial state cleanup failed";
    };
    actions.cleanup_confirmed = [&] { return cleanup_confirmed; };
    actions.recover_start = [&] {
        const bool recovered = run_soft_reboot_command("start");
        if (!recovered)
            error += "; Android recovery start failed";
        return recovered;
    };
    actions.recover_services = [&] {
        std::string service_error;
        const bool recovered = on_services(&service_error) == 0;
        if (!recovered)
            error += "; module service recovery failed: " + service_error;
        return recovered;
    };
    actions.restore_boot_flag = [&] {
        if (!set_boot_completed("1"))
            error += "; cannot restore boot-completed property";
    };
    const auto result = run_soft_reboot_flow(actions);
    if (result.ok) {
        (void)reboot::record("complete", pending ? "completed_pending" : "completed", {}, pending);
    } else {
        error = std::string("failed during ") + soft_reboot_stage_name(result.failed_stage) +
                (error.empty() ? "" : ": " + error);
        if (result.recovery_deferred)
            error += "; controller completion unconfirmed; recovery deferred to the controller";
        (void)reboot::record(soft_reboot_stage_name(result.failed_stage),
                             result.recovery_deferred ? "accepted" : "failed", error, pending);
        LOGE("Soft reboot: %s", error.c_str());
    }
    reboot::release();
    _exit(result.ok ? 0 : 1);
}

}  // namespace ksud
