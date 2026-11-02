#include "kagami/embedded.hpp"
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/ksucalls.hpp"
#include "core/log.hpp"
#include "core/reboot_state.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "module/metamodule.hpp"
#include "mount/backend.hpp"
#include "mount/kasumi.hpp"
#include "terminal.hpp"
#include "utils.hpp"

namespace kagami {
int embedded_command(const std::vector<std::string>& args) try {
    if (!args.empty() && (args[0] == "version" || args[0] == "--version"))
        return run_command(args);
    if (geteuid() != 0) {
        return ksud::terminal::error("Kagami requires root",
                                     "Run this command from an authorized root shell.");
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h" ||
        args[0] == "version" || args[0] == "--version" || args[0] == "daemon")
        return run_command(args);
    return run_via_daemon(args, true);
} catch (const std::exception& error) {
    return ksud::terminal::error(std::string("Kagami: ") + error.what());
}

int embedded_mount() try {
    std::string error;
    if (!prepare_runtime(error)) {
        logging::write(logging::Level::Error, "boot", error);
        return 1;
    }
    if (!std::filesystem::exists(runtime_config_file()) && !write_default_config(error)) {
        std::cerr << "Kagami: " << error << '\n';
        return 1;
    }
    Config config;
    if (!read_config_file(config, error)) {
        std::cerr << "Kagami: " << error << '\n';
        return 1;
    }
    if (!config.builtin_mount_enabled) {
        logging::write(logging::Level::Info, "boot", "built-in mounting disabled; skip mount plan");
        return 0;
    }
    logging::write(logging::Level::Info, "boot",
                   "starting built-in mount plan; no external LKM lookup");
    return run_via_daemon({"module", "mount-all"});
} catch (const std::exception& error) {
    std::cerr << "Kagami mount: " << error.what() << '\n';
    return 1;
}

void embedded_boot_completed() {
    logging::write(logging::Level::Info, "boot", "boot completed");
    if (!reboot::authorized())
        mount::recovery_boot_completed();
    if (ksud::is_safe_mode() || !embedded_external_mount_owner().empty()) {
        return;
    }
    if (run_via_daemon({"hide", "apply"}) != 0) {
        logging::write(logging::Level::Error, "boot", "failed to restore user hide rules");
    }
}

bool embedded_verify_hide_rules(int& pending, std::string& error) {
    return mount::kasumi::verify_persisted_hide_rules(pending, error);
}

void embedded_post_fs_data() {
    std::string error;
    if (!prepare_runtime(error) || !logging::prepare_boot_log(error)) {
        logging::write(logging::Level::Error, "boot", "prepare built-in controller: " + error);
        return;
    }
    logging::write(logging::Level::Info, "boot",
                   "post-fs-data; state=" + runtime_data_dir().string());
}

void embedded_mount_skipped(const std::string& reason) {
    logging::write(logging::Level::Info, "boot", "built-in mount skipped: " + reason);
}

bool embedded_register_umount(const std::string& path) {
    const bool ok = ksud::umount_list_add(path, 0) == 0;
    if (!ok)
        logging::write(logging::Level::Error, "mount", "failed to register unmount path " + path);
    return ok;
}

bool embedded_unregister_umount(const std::string& path) {
    return ksud::umount_list_del(path) == 0;
}

std::string embedded_external_mount_owner() {
    return ksud::metamodule_mount_owner();
}
}  // namespace kagami

extern "C" int ksu_kasumi_ioctl(unsigned long cmd, void* arg) {
    return ksud::ksuctl(static_cast<int>(cmd), arg);
}
