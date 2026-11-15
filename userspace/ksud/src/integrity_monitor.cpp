#include "integrity_monitor.hpp"

#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

namespace ksud {

namespace {

constexpr const char* kMonitorLockPath = "/data/adb/ksu/ksud-integrity.lock";
constexpr const char* kMonitorStatePath = "/data/adb/ksu/ksud-integrity.json";
constexpr const char* kMonitorStateTemporaryPath = "/data/adb/ksu/.ksud-integrity.tmp";
constexpr const char* kBroadcastAction = "org.rouyashiki.yukisu.action.KSUD_INTEGRITY_CHANGED";
constexpr uint64_t kMaximumDaemonSize = 128ULL * 1024ULL * 1024ULL;
constexpr int kSettleMilliseconds = 300;
constexpr int kReadyTimeoutMilliseconds = 5000;

struct FileSnapshot {
    bool exists = false;
    bool valid = false;
    dev_t device = 0;
    ino_t inode = 0;
    off_t size = 0;
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string digest;
    std::string error;
};

bool write_all(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void report_ready(int* fd, bool ready) {
    if (*fd < 0)
        return;
    const char value = ready ? '1' : '0';
    (void)write_all(*fd, std::string_view(&value, 1));
    close(*fd);
    *fd = -1;
}

std::string hex_digest(const std::array<unsigned char, 32>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output(digest.size() * 2, '0');
    for (size_t index = 0; index < digest.size(); ++index) {
        output[index * 2] = hex[digest[index] >> 4];
        output[(index * 2) + 1] = hex[digest[index] & 0x0f];
    }
    return output;
}

bool hash_file(int fd, off_t size, std::string* output) {
    if (size < 0 || static_cast<uint64_t>(size) > kMaximumDaemonSize)
        return false;

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    std::array<unsigned char, 65536> buffer{};
    off_t offset = 0;
    while (ok && offset < size) {
        const size_t requested =
            static_cast<size_t>(std::min<off_t>(static_cast<off_t>(buffer.size()), size - offset));
        const ssize_t count = pread(fd, buffer.data(), requested, offset);
        if (count > 0) {
            ok = mbedtls_sha256_update(&context, buffer.data(), static_cast<size_t>(count)) == 0;
            offset += count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        ok = false;
    }

    std::array<unsigned char, 32> digest{};
    if (ok)
        ok = mbedtls_sha256_finish(&context, digest.data()) == 0;
    mbedtls_sha256_free(&context);
    if (ok)
        *output = hex_digest(digest);
    return ok;
}

FileSnapshot inspect_fd(int fd, bool require_secure_metadata) {
    FileSnapshot snapshot;
    snapshot.exists = fd >= 0 || errno != ENOENT;
    if (fd < 0) {
        snapshot.error = errno == ENOENT ? "missing" : strerror(errno);
        return snapshot;
    }

    struct stat before{};
    if (fstat(fd, &before) != 0) {
        snapshot.error = strerror(errno);
        return snapshot;
    }
    snapshot.device = before.st_dev;
    snapshot.inode = before.st_ino;
    snapshot.size = before.st_size;
    snapshot.mode = before.st_mode;
    snapshot.uid = before.st_uid;
    snapshot.gid = before.st_gid;

    if (!S_ISREG(before.st_mode)) {
        snapshot.error = "not_regular";
        return snapshot;
    }
    if (require_secure_metadata &&
        (before.st_uid != 0 || before.st_gid != 0 || (before.st_mode & 0777) != 0755)) {
        snapshot.error = "unsafe_metadata";
        return snapshot;
    }
    if (!hash_file(fd, before.st_size, &snapshot.digest)) {
        snapshot.error = "hash_failed";
        return snapshot;
    }

    struct stat after{};
    if (fstat(fd, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        snapshot.digest.clear();
        snapshot.error = "changed_while_reading";
        return snapshot;
    }
    snapshot.valid = true;
    return snapshot;
}

FileSnapshot inspect_path(int directory_fd) {
    const int fd = openat(directory_fd, "ksud", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    FileSnapshot snapshot = inspect_fd(fd, true);
    if (fd >= 0)
        close(fd);
    return snapshot;
}

bool matches_baseline(const FileSnapshot& baseline, const FileSnapshot& current) {
    return baseline.valid && current.valid && baseline.digest == current.digest;
}

std::string snapshot_key(const FileSnapshot& snapshot) {
    std::string key;
    key.reserve(snapshot.digest.size() + snapshot.error.size() + 96);
    append_uint(&key, snapshot.exists ? 1 : 0);
    key.push_back(':');
    append_uint(&key, snapshot.valid ? 1 : 0);
    key.push_back(':');
    append_uint(&key, static_cast<uint64_t>(snapshot.device));
    key.push_back(':');
    append_uint(&key, static_cast<uint64_t>(snapshot.inode));
    key.push_back(':');
    append_uint(&key, static_cast<uint64_t>(snapshot.mode));
    key.push_back(':');
    append_uint(&key, static_cast<uint64_t>(snapshot.uid));
    key.push_back(':');
    key.append(snapshot.digest);
    key.push_back(':');
    key.append(snapshot.error);
    return key;
}

const char* mismatch_reason(const FileSnapshot& baseline, const FileSnapshot& current) {
    if (!current.exists)
        return "missing";
    if (!current.valid)
        return current.error.empty() ? "invalid" : current.error.c_str();
    if (!baseline.valid)
        return "baseline_invalid";
    if (baseline.digest != current.digest)
        return "content_changed";
    return "metadata_changed";
}

std::string read_boot_id() {
    const auto value = read_file("/proc/sys/kernel/random/boot_id");
    return value ? std::string(trim_view(*value)) : std::string{};
}

void append_json_string(std::string* output, std::string_view value) {
    output->push_back('"');
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            output->append("\\\\");
            break;
        case '"':
            output->append("\\\"");
            break;
        case '\n':
            output->append("\\n");
            break;
        case '\r':
            output->append("\\r");
            break;
        case '\t':
            output->append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(ch) >= 0x20)
                output->push_back(ch);
            break;
        }
    }
    output->push_back('"');
}

void append_snapshot_json(std::string* output, const FileSnapshot& snapshot) {
    output->append("{\"exists\":");
    output->append(snapshot.exists ? "true" : "false");
    output->append(",\"valid\":");
    output->append(snapshot.valid ? "true" : "false");
    output->append(",\"device\":");
    append_uint(output, static_cast<uint64_t>(snapshot.device));
    output->append(",\"inode\":");
    append_uint(output, static_cast<uint64_t>(snapshot.inode));
    output->append(",\"size\":");
    append_int(output, static_cast<int64_t>(snapshot.size));
    output->append(",\"mode\":");
    append_uint(output, static_cast<uint64_t>(snapshot.mode & 07777));
    output->append(",\"uid\":");
    append_uint(output, static_cast<uint64_t>(snapshot.uid));
    output->append(",\"gid\":");
    append_uint(output, static_cast<uint64_t>(snapshot.gid));
    output->append(",\"sha256\":");
    append_json_string(output, snapshot.digest);
    output->append(",\"error\":");
    append_json_string(output, snapshot.error);
    output->push_back('}');
}

bool write_state(const FileSnapshot& baseline, const FileSnapshot& current, bool mismatch) {
    std::string json;
    json.reserve(768);
    json.append("{\"schema\":1,\"mismatch\":");
    json.append(mismatch ? "true" : "false");
    json.append(",\"detected_at\":");
    append_int(&json, static_cast<int64_t>(time(nullptr)));
    json.append(",\"boot_id\":");
    append_json_string(&json, read_boot_id());
    json.append(",\"reason\":");
    append_json_string(&json, mismatch ? mismatch_reason(baseline, current) : "restored");
    json.append(",\"baseline\":");
    append_snapshot_json(&json, baseline);
    json.append(",\"current\":");
    append_snapshot_json(&json, current);
    json.append("}\n");

    const int fd = open(kMonitorStateTemporaryPath,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    const bool ok = fchmod(fd, 0600) == 0 && write_all(fd, json) && fsync(fd) == 0;
    const int saved_errno = errno;
    close(fd);
    if (!ok) {
        errno = saved_errno;
        (void)unlink(kMonitorStateTemporaryPath);
        return false;
    }
    if (rename(kMonitorStateTemporaryPath, kMonitorStatePath) != 0) {
        (void)unlink(kMonitorStateTemporaryPath);
        return false;
    }

    const int directory_fd = open(WORKING_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        (void)fsync(directory_fd);
        close(directory_fd);
    }
    return true;
}

std::string manager_package(uint32_t uid) {
    std::string package;
    (void)for_each_file_line("/data/system/packages.list", [&](std::string_view line) {
        std::string_view rest = line;
        const std::string_view name = next_token(&rest);
        uint32_t package_uid = 0;
        if (!name.empty() && parse_uint32(next_token(&rest), &package_uid) && package_uid == uid) {
            package.assign(name);
            return false;
        }
        return true;
    });
    return package;
}

void broadcast_manager(const std::string& package, uint32_t user_id) {
    const ExecResult result =
        exec_command({"am", "broadcast", "--user", std::to_string(user_id), "-f", "0x10000020",
                      "-p", package, "-a", kBroadcastAction});
    if (result.exit_code != 0) {
        LOGW("ksud integrity: manager broadcast failed for %s: %s", package.c_str(),
             result.stderr_str.c_str());
    }
}

void notify_manager() {
    const int uid = get_manager_uid();
    const uint32_t manager_uid = uid >= 0 ? static_cast<uint32_t>(uid) : 0;
    const uint32_t user_id = manager_uid / 100000;
    const std::string package = manager_package(manager_uid);
    if (!package.empty())
        broadcast_manager(package, user_id);
    if (package != DEFAULT_PACKAGE_NAME)
        broadcast_manager(DEFAULT_PACKAGE_NAME, user_id);
}

void publish_change(const FileSnapshot& baseline, const FileSnapshot& current, bool mismatch) {
    if (!write_state(baseline, current, mismatch)) {
        LOGW("ksud integrity: cannot persist state: %s", strerror(errno));
    }
    if (mismatch) {
        LOGW("ksud integrity: daemon mismatch detected (%s)", mismatch_reason(baseline, current));
    } else {
        LOGI("ksud integrity: daemon restored");
    }
    notify_manager();
}

bool event_is_relevant(const struct inotify_event& event) {
    if ((event.mask & IN_Q_OVERFLOW) != 0)
        return true;
    return event.len > 0 && strcmp(event.name, "ksud") == 0;
}

bool drain_events(int fd, bool* relevant) {
    alignas(struct inotify_event) std::array<char, 16384> buffer{};
    for (;;) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                return true;
            return false;
        }
        if (count == 0)
            return false;

        size_t offset = 0;
        while (offset + sizeof(struct inotify_event) <= static_cast<size_t>(count)) {
            const auto* event =
                reinterpret_cast<const struct inotify_event*>(buffer.data() + offset);
            if (event_is_relevant(*event))
                *relevant = true;
            offset += sizeof(struct inotify_event) + event->len;
        }
    }
}

int remaining_milliseconds(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0)
        return 0;
    return remaining > INT32_MAX ? INT32_MAX : static_cast<int>(remaining);
}

bool wait_for_settled_change(int fd) {
    struct pollfd event{fd, POLLIN, 0};
    for (;;) {
        int result = poll(&event, 1, -1);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;

        bool relevant = false;
        if (!drain_events(fd, &relevant))
            return false;
        if (!relevant)
            continue;

        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSettleMilliseconds);
        for (;;) {
            event.revents = 0;
            result = poll(&event, 1, remaining_milliseconds(deadline));
            if (result < 0 && errno == EINTR)
                continue;
            if (result == 0)
                return true;
            if (result < 0)
                return false;

            bool more_relevant = false;
            if (!drain_events(fd, &more_relevant))
                return false;
            if (more_relevant) {
                deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(kSettleMilliseconds);
            }
        }
    }
}

int acquire_monitor_lock() {
    const int fd = open(kMonitorLockPath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int run_monitor(int ready_fd) {
    (void)prctl(PR_SET_NAME, "ksud-integrity", 0, 0, 0);
    const int lock_fd = acquire_monitor_lock();
    if (lock_fd < 0) {
        if (errno == EWOULDBLOCK) {
            report_ready(&ready_fd, true);
            return 0;
        }
        LOGW("ksud integrity: cannot acquire lock: %s", strerror(errno));
        report_ready(&ready_fd, false);
        return 1;
    }

    const int self_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    const FileSnapshot baseline = inspect_fd(self_fd, false);
    if (self_fd >= 0)
        close(self_fd);
    if (!baseline.valid) {
        LOGW("ksud integrity: cannot read baseline: %s", baseline.error.c_str());
        report_ready(&ready_fd, false);
        close(lock_fd);
        return 1;
    }

    const int directory_fd = open(ADB_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    // A file watch follows the old inode and misses an atomic replacement.
    constexpr uint32_t mask =
        IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_CLOSE_WRITE | IN_ATTRIB;
    const int watch = inotify_fd >= 0 ? inotify_add_watch(inotify_fd, ADB_DIR, mask) : -1;
    if (directory_fd < 0 || inotify_fd < 0 || watch < 0) {
        LOGW("ksud integrity: cannot initialize directory watch: %s", strerror(errno));
        report_ready(&ready_fd, false);
        if (directory_fd >= 0)
            close(directory_fd);
        if (inotify_fd >= 0)
            close(inotify_fd);
        close(lock_fd);
        return 1;
    }

    const FileSnapshot initial = inspect_path(directory_fd);
    std::string current_key = snapshot_key(initial);
    bool current_mismatch = !matches_baseline(baseline, initial);
    report_ready(&ready_fd, true);
    if (current_mismatch)
        publish_change(baseline, initial, true);

    while (wait_for_settled_change(inotify_fd)) {
        const FileSnapshot next = inspect_path(directory_fd);
        std::string next_key = snapshot_key(next);
        if (next_key == current_key)
            continue;

        const bool next_mismatch = !matches_baseline(baseline, next);
        if (next_mismatch || current_mismatch)
            publish_change(baseline, next, next_mismatch);
        current_key = std::move(next_key);
        current_mismatch = next_mismatch;
    }

    LOGW("ksud integrity: directory watch stopped: %s", strerror(errno));
    close(inotify_fd);
    close(directory_fd);
    close(lock_fd);
    return 1;
}

void redirect_standard_streams() {
    const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull < 0)
        return;
    (void)dup2(devnull, STDIN_FILENO);
    (void)dup2(devnull, STDOUT_FILENO);
    (void)dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
        close(devnull);
}

}  // namespace

bool start_ksud_integrity_monitor() {
    if (!ensure_dir_exists(WORKING_DIR)) {
        LOGW("ksud integrity: cannot create working directory");
        return false;
    }

    std::array<int, 2> ready_pipe{};
    if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
        LOGW("ksud integrity: readiness pipe failed: %s", strerror(errno));
        return false;
    }

    const pid_t launcher = fork();
    if (launcher < 0) {
        LOGW("ksud integrity: fork failed: %s", strerror(errno));
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return false;
    }
    if (launcher == 0) {
        close(ready_pipe[0]);
        // Keep the old executable mapping; the monitored path may already be replaced.
        detach_process_group(false);
        switch_cgroups();
        redirect_standard_streams();

        const pid_t daemon = fork();
        if (daemon < 0) {
            report_ready(&ready_pipe[1], false);
            _exit(127);
        }
        if (daemon > 0)
            _exit(0);

        _exit(run_monitor(ready_pipe[1]));
    }

    close(ready_pipe[1]);
    int launcher_status = 0;
    while (waitpid(launcher, &launcher_status, 0) < 0 && errno == EINTR) {
    }

    struct pollfd ready{ready_pipe[0], POLLIN, 0};
    int poll_result;
    do {
        poll_result = poll(&ready, 1, kReadyTimeoutMilliseconds);
    } while (poll_result < 0 && errno == EINTR);

    char value = '0';
    const ssize_t count = poll_result > 0 ? read(ready_pipe[0], &value, 1) : -1;
    close(ready_pipe[0]);
    const bool started = count == 1 && value == '1';
    if (!started)
        LOGW("ksud integrity: monitor did not become ready");
    return started;
}

}  // namespace ksud
