#include "core/runtime.hpp"
#include "defs.hpp"
#include "utils.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#if defined(__ANDROID__)
#include <sys/xattr.h>
#endif
#include "kagami/embedded_paths.hpp"

namespace kagami {

namespace fs = std::filesystem;

fs::path runtime_data_dir() {
    return embedded_data_dir;
}

fs::path runtime_modules_dir() {
    return fs::path(ksud::MODULE_DIR).lexically_normal();
}

fs::path runtime_config_file() {
    return runtime_data_dir() / "config.json";
}

fs::path runtime_log_file() {
    return embedded_log_file;
}

fs::path runtime_socket_file() {
    return runtime_data_dir() / "kagamid.sock";
}
fs::path runtime_pid_file() {
    return runtime_data_dir() / "kagamid.pid";
}
fs::path runtime_daemon_lock_file() {
    return runtime_data_dir() / "kagamid.lock";
}
std::string runtime_boot_id() {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(input, id);
    return id;
}

bool marker_matches_current_boot(const fs::path& path) {
    const std::string boot_id = runtime_boot_id();
    std::ifstream input(path);
    std::string recorded;
    return !boot_id.empty() && std::getline(input, recorded) && recorded == boot_id;
}

bool write_boot_marker(const fs::path& path, const std::string& detail) {
    const std::string boot_id = runtime_boot_id();
    return !boot_id.empty() &&
           ksud::write_file_atomic(path, boot_id + "\n" + (detail.empty() ? "" : detail + "\n"));
}

namespace {
bool metadata(const fs::path& path, mode_t mode, std::string& error) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        error = "refusing metadata symlink: " + path.string();
        return false;
    }
    if (((st.st_mode & 07777) != mode && chmod(path.c_str(), mode) != 0) ||
        (geteuid() == 0 && (st.st_uid != 0 || st.st_gid != 0) && chown(path.c_str(), 0, 0) != 0)) {
        error = "secure " + path.string() + ": " + std::strerror(errno);
        return false;
    }
#if defined(__ANDROID__)
    constexpr char context[] = "u:object_r:adb_data_file:s0";
    char current[256]{};
    const ssize_t length =
        lgetxattr(path.c_str(), "security.selinux", current, sizeof(current) - 1);
    if (length < 0 || std::strcmp(current, context) != 0) {
        if (lsetxattr(path.c_str(), "security.selinux", context, sizeof(context), 0) != 0) {
            error = "label " + path.string() + ": " + std::strerror(errno);
            return false;
        }
    }
#endif
    return true;
}
}  // namespace

bool prepare_private_directory(const fs::path& path, std::string& error) {
    error.clear();
    const auto normalized = path.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute() || normalized == normalized.root_path() ||
        normalized == "/data" || normalized == "/data/adb" || normalized == "/data/adb/ksu" ||
        normalized == "/data/adb/modules" || normalized == "/tmp") {
        error = "refusing a shared runtime directory: " + path.string();
        return false;
    }
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        error = "mkdir " + path.string() + ": " + ec.message();
        return false;
    }
    return metadata(path, 0700, error);
}

bool prepare_private_file(const fs::path& path, std::string& error) {
    error.clear();
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT)
            return true;
        error = "lstat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    if ((!S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode)) || st.st_nlink != 1) {
        error = "not a private controller file: " + path.string();
        return false;
    }
    return metadata(path, 0600, error);
}

bool prepare_runtime(std::string& error) {
    error.clear();
    if (!prepare_private_directory(runtime_data_dir(), error) ||
        !prepare_private_directory(runtime_data_dir() / "run", error) ||
        !prepare_private_directory(runtime_log_file().parent_path(), error))
        return false;
    // These are controller records, not mounted module payloads or LKM assets.
    for (const auto* name :
         {"config.json", "config.json.lock", "module_mode.json", "module_bypass.json",
          "module_rules.json", "user_hide_rules.json", "hide_bypass.json", "kagamid.pid",
          "kagamid.sock", "kagamid.lock", "mirror.img", "mirror.erofs"}) {
        if (!prepare_private_file(runtime_data_dir() / name, error))
            return false;
    }
    for (const auto* name : {"soft_reboot.lock", "soft_reboot.json", "mount_cycle"}) {
        if (!prepare_private_file(runtime_data_dir() / "run" / name, error))
            return false;
    }
    return true;
}

}  // namespace kagami
