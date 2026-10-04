#include "su_path.hpp"
#include "../../../common/su_path.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../magisk_compat/su_transition.hpp"
#include "../terminal.hpp"
#include "../utils.hpp"
#include "ksucalls.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ksud {
namespace {

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

bool supported_config(const ksu_su_path_config& config) {
    return config.version == KSU_SU_PATH_VERSION && config.size == sizeof(config) &&
           config.reserved == 0 && (config.flags & ~KSU_SU_PATH_ENABLED) == 0 &&
           strnlen(config.path, sizeof(config.path)) < sizeof(config.path) &&
           validate_su_path(config.path) == 0;
}

ksu_su_path_config path_config(const std::string& path) {
    ksu_su_path_config config{};
    config.version = KSU_SU_PATH_VERSION;
    config.size = sizeof(config);
    path.copy(config.path, sizeof(config.path) - 1);
    return config;
}

int read_path(std::string& path) {
    const Fd fd(open(SU_PATH_CONFIG_PATH, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.get() < 0) {
        if (errno != ENOENT)
            return -errno;
        path = KSU_SU_PATH_DEFAULT;
        return 0;
    }
    struct stat info{};
    if (fstat(fd.get(), &info) != 0)
        return -errno;
    if (!S_ISREG(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0077) != 0)
        return -EACCES;
    std::array<char, KSU_SU_PATH_MAX + 2> buffer{};
    size_t used = 0;
    while (used < buffer.size()) {
        const auto count = read(fd.get(), buffer.data() + used, buffer.size() - used);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (count == 0)
            break;
        used += static_cast<size_t>(count);
    }
    if (used == buffer.size())
        return -ENAMETOOLONG;
    path.assign(buffer.data(), used);
    return parse_su_path_file(path);
}

int write_all(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const auto count = write(fd, data.data() + offset, data.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (count == 0)
            return -EIO;
        offset += static_cast<size_t>(count);
    }
    return 0;
}

int save_path_locked(const std::string& path, bool* persistence_error = nullptr) {
    if (persistence_error != nullptr)
        *persistence_error = false;
    int ret = validate_su_path(path);
    if (ret)
        return ret;
    const auto [kasumi, supported] = get_feature(KSU_FEATURE_KASUMI);
    if (path != KSU_SU_PATH_DEFAULT && supported && kasumi == 0)
        return -EOPNOTSUPP;
    ksu_su_path_config previous{};
    ret = get_su_path_config(&previous);
    if (ret)
        return ret;
    if (!supported_config(previous))
        return -EOPNOTSUPP;
    if (persistence_error != nullptr)
        *persistence_error = true;
    if (!ensure_dir_exists(WORKING_DIR))
        return -EIO;
    const Fd directory(open(WORKING_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0)
        return -errno;

    std::string temporary = std::string(SU_PATH_CONFIG_PATH) + ".tmp.XXXXXX";
    const Fd fd(mkostemp(temporary.data(), O_CLOEXEC));
    if (fd.get() < 0)
        return -errno;
    if (fchmod(fd.get(), 0600) != 0 || fchown(fd.get(), 0, 0) != 0)
        ret = -errno;
    if (!ret)
        ret = write_all(fd.get(), path + '\n');
    if (!ret && fsync(fd.get()) != 0)
        ret = -errno;
    if (ret) {
        unlink(temporary.c_str());
        return ret;
    }

    if (persistence_error != nullptr)
        *persistence_error = false;
    ret = set_su_path_config(path_config(path));
    if (ret) {
        unlink(temporary.c_str());
        return ret;
    }
    if (persistence_error != nullptr)
        *persistence_error = true;
    if (rename(temporary.c_str(), SU_PATH_CONFIG_PATH) != 0) {
        ret = -errno;
        previous.flags = 0;
        if (set_su_path_config(previous) != 0)
            LOGE("Failed to restore the previous su path after persistence failure");
        unlink(temporary.c_str());
        return ret;
    }
    // After rename, disk and kernel agree even if durability cannot be confirmed.
    if (fsync(directory.get()) != 0) {
        ret = -errno;
        LOGE("su path was applied, but syncing its configuration directory failed");
        return ret;
    }
    return 0;
}

int save_path(const std::string& path, bool* persistence_error = nullptr) {
    const SucompatTransitionLock transition;
    if (!transition.locked())
        return -EBUSY;
    return save_path_locked(path, persistence_error);
}

const char* save_error_code(int result, bool persistence_error) {
    if (result == 0)
        return "none";
    if (persistence_error)
        return "persistence_failed";
    switch (-result) {
    case EEXIST:
        return "path_exists";
    case ENOENT:
    case ENOTDIR:
        return "parent_unavailable";
    case EINVAL:
    case ELOOP:
        return "invalid_path";
    case ENAMETOOLONG:
        return "path_too_long";
    case EACCES:
    case EPERM:
    case EROFS:
        return "permission_denied";
    case ENOTTY:
    case EOPNOTSUPP:
        return "unsupported";
    case EAGAIN:
    case EBUSY:
        return "busy";
    default:
        return "failed";
    }
}

}  // namespace

int reset_su_path_locked() {
    ksu_su_path_config current{};
    const int ret = get_su_path_config(&current);
    if (ret == -ENOTTY || ret == -EOPNOTSUPP)
        return 0;
    if (ret)
        return ret;
    return save_path_locked(KSU_SU_PATH_DEFAULT);
}

int restore_su_path(bool* custom) {
    if (custom != nullptr)
        *custom = false;
    std::string path;
    int ret = read_path(path);
    if (ret) {
        LOGE("Cannot read persisted su path: %s", strerror(-ret));
        return ret;
    }
    const bool is_custom = path != KSU_SU_PATH_DEFAULT;
    if (custom != nullptr)
        *custom = is_custom;
    ksu_su_path_config current{};
    ret = get_su_path_config(&current);
    if (!is_custom && (ret == -ENOTTY || ret == -EOPNOTSUPP))
        return 0;
    if (ret)
        return ret;
    if (!supported_config(current))
        return -EOPNOTSUPP;
    if (path == current.path)
        return 0;
    return set_su_path_config(path_config(path));
}

int su_path_command(const std::vector<std::string>& args) {
    int ret = -EINVAL;
    if (args.size() == 3 && args[0] == "set" && args[1] == "--json") {
        bool persistence_error = false;
        ret = save_path(args[2], &persistence_error);
        if (printf("{\"version\":1,\"error\":\"%s\",\"errno\":%d}\n",
                   save_error_code(ret, persistence_error), -ret) < 0)
            return 1;
        return ret ? 1 : 0;
    }
    if (args.size() == 1 && args[0] == "get") {
        ksu_su_path_config config{};
        ret = get_su_path_config(&config);
        if (!ret && !supported_config(config))
            ret = -EOPNOTSUPP;
        if (!ret && puts(config.path) == EOF)
            ret = -EIO;
    } else if (args.size() == 2 && args[0] == "set") {
        ret = save_path(args[1]);
    } else if (args.size() == 1 && args[0] == "reset") {
        ret = save_path(KSU_SU_PATH_DEFAULT);
    } else {
        (void)fprintf(stderr, "Usage: ksud su-path get | set [--json] <absolute-path> | reset\n");
    }
    if (ret)
        (void)terminal::errorf("cannot update or read the su path: %s (%d)", strerror(-ret), -ret);
    return ret ? 1 : 0;
}

}  // namespace ksud
