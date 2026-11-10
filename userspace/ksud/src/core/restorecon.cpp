#include "restorecon.hpp"
#include <sys/xattr.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <optional>
#include <vector>
#include "../defs.hpp"
#include "../log.hpp"

namespace fs = std::filesystem;

namespace ksud {

static constexpr const char* SELINUX_XATTR = "security.selinux";

bool lsetfilecon(const fs::path& path, const std::string& con) {
    const int ret = lsetxattr(path.c_str(), SELINUX_XATTR, con.c_str(), con.length() + 1, 0);
    if (ret != 0) {
        LOGW("Failed to set SELinux context for %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

namespace {

std::optional<std::string> read_file_context(const fs::path& path) {
    std::array<char, 256> buf{};
    const ssize_t len = lgetxattr(path.c_str(), SELINUX_XATTR, buf.data(), buf.size() - 1);
    if (len < 0) {
        return std::nullopt;
    }
    buf[static_cast<size_t>(len)] = '\0';
    return std::string(buf.data());
}

}  // namespace

std::string lgetfilecon(const fs::path& path) {
    return read_file_context(path).value_or("");
}

bool setsyscon(const fs::path& path) {
    return lsetfilecon(path, SYSTEM_CON);
}

bool restore_syscon(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return !ec;
    }

    if (!setsyscon(dir))
        return false;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        if (!setsyscon(it->path())) {
            return false;
        }
    }
    if (ec) {
        LOGE("Error walking directory %s: %s", dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool restore_syscon_if_unlabeled(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return !ec;
    }

    const auto restore = [](const fs::path& path) {
        const auto con = read_file_context(path);
        if (path == DAEMON_PATH)
            return con == KSU_CON || lsetfilecon(path, KSU_CON);
        return !con || (!con->empty() && *con != UNLABEL_CON) || lsetfilecon(path, SYSTEM_CON);
    };
    if (!restore(dir))
        return false;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        if (!restore(it->path()))
            return false;
    }
    if (ec) {
        LOGE("Error walking directory %s: %s", dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

bool restorecon() {
    bool success = true;

    // Set context for daemon
    if (!lsetfilecon(DAEMON_PATH, KSU_CON)) {
        LOGW("Failed to set context for daemon");
        success = false;
    }

    // Restore module directory contexts
    if (!restore_syscon_if_unlabeled(MODULE_DIR)) {
        LOGW("Failed to restore contexts for module directory");
        success = false;
    }

    return success;
}

bool restorecon(const fs::path& path, bool recursive) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        LOGW("Path does not exist: %s", path.c_str());
        return false;
    }

    const char* context = SYSTEM_CON;
    if (path == DAEMON_PATH)
        context = KSU_CON;
    else if (path == "/data/adb")
        context = ADB_CON;

    if (recursive) {
        const bool directory = fs::is_directory(path, ec);
        if (ec) {
            LOGW("Failed to inspect %s: %s", path.c_str(), ec.message().c_str());
            return false;
        }
        if (directory)
            return restore_syscon_if_unlabeled(path);
    }
    return lsetfilecon(path, context);
}

}  // namespace ksud
