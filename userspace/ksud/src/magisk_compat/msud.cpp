#include "magisk_compat/msud.hpp"

#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "magisk_compat/su_transition.hpp"
#include "utils.hpp"

extern "C" {
#include "uapi/feature.h"
#include "uapi/supercall.h"
}

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace ksud {

namespace {

static_assert(sizeof(ksu_su_prompt_request) == 56, "su prompt request ABI drift");

constexpr const char* kManagerActivityClass = "org.rouyashiki.yukisu.ui.SuRequestActivity";
constexpr const char* kMsudLockPath = "/data/adb/ksu/msud.lock";
constexpr uint32_t kPerUserRange = 100000U;
constexpr uint32_t kMsudStateMagic = 0x4D535553U;  // "MSUS"
constexpr uint32_t kMsudStateVersion = 2;
constexpr int kReadyTimeoutMs = 5000;
constexpr int kPromptLaunchTimeoutMs = 5000;
constexpr int kStopGracePolls = 50;
constexpr int kStopKillPolls = 50;
constexpr useconds_t kStopPollIntervalUs = 20000;

struct MsudState {
    uint32_t magic;
    uint32_t version;
    int32_t pid;
    uint32_t reserved;
    uint64_t start_time;
    uint64_t exe_dev;
    uint64_t exe_ino;
};

// NOLINTNEXTLINE(cert-dcl50-cpp)
void mlog(const char* format, ...) {
    FILE* file = fopen("/data/adb/ksu/log/msud.log", "ae");
    if (file == nullptr) {
        return;
    }
    struct timespec timestamp{};
    (void)clock_gettime(CLOCK_REALTIME, &timestamp);
    (void)fprintf(file, "[%ld.%03ld pid %d] ", static_cast<long>(timestamp.tv_sec),
                  timestamp.tv_nsec / 1000000, getpid());
    va_list args;
    va_start(args, format);
    (void)vfprintf(file, format, args);
    va_end(args);
    (void)fputc('\n', file);
    (void)fclose(file);
}

bool write_all(int fd, const void* buffer, size_t size) {
    const auto* data = static_cast<const unsigned char*>(buffer);
    size_t written = 0;
    while (written < size) {
        const ssize_t count = write(fd, data + written, size - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        written += static_cast<size_t>(count);
    }
    return true;
}

bool run_prompt_command(const std::vector<std::string>& arguments) {
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        (void)setsid();
        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kPromptLaunchTimeoutMs);
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (waited < 0 && errno != EINTR) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            (void)kill(child, SIGKILL);
            (void)kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        (void)poll(nullptr, 0, static_cast<int>(std::min<int64_t>(remaining, 20)));
    }
}

std::string uid_to_package(uint32_t uid) {
    std::string package;
    const uint32_t app_id = uid % kPerUserRange;
    (void)for_each_file_line("/data/system/packages.list", [&](std::string_view line) {
        std::string_view rest = line;
        const std::string_view name = next_token(&rest);
        const std::string_view uid_text = next_token(&rest);
        uint32_t package_uid = 0;
        if (!name.empty() && parse_uint32(uid_text, &package_uid) &&
            package_uid % kPerUserRange == app_id) {
            package.assign(name);
            return false;
        }
        return true;
    });
    return package;
}

bool launch_prompt(const ksu_su_prompt_request& request) {
    const int manager_uid = get_manager_uid();
    const std::string manager_package =
        manager_uid >= 0 ? uid_to_package(static_cast<uint32_t>(manager_uid)) : std::string{};
    if (manager_package.empty()) {
        mlog("msud: manager package unavailable for request %llu",
             static_cast<unsigned long long>(request.request_id));
        return false;
    }

    const int user_id = static_cast<int>(request.uid / kPerUserRange);
    const std::string component = manager_package + "/" + kManagerActivityClass;
    const std::string comm(request.comm, strnlen(request.comm, sizeof(request.comm)));
    const std::vector<std::string> arguments = {
        "/system/bin/am",
        "start",
        "--user",
        std::to_string(user_id),
        "-n",
        component,
        "-a",
        "android.intent.action.VIEW",
        "-f",
        "0x18800020",
        "--el",
        "ksu.req_id",
        std::to_string(request.request_id),
        "--el",
        "ksu.nonce",
        std::to_string(request.nonce),
        "--ei",
        "ksu.uid",
        std::to_string(request.uid),
        "--es",
        "ksu.comm",
        comm,
    };
    mlog("msud: prompt request=%llu uid=%u pid=%u tgid=%u comm=%s",
         static_cast<unsigned long long>(request.request_id), request.uid, request.pid,
         request.tgid, comm.c_str());
    return run_prompt_command(arguments);
}

bool valid_prompt_request(const ksu_su_prompt_request& request) {
    return request.version == KSU_SU_PROMPT_VERSION && request.size == sizeof(request) &&
           request.request_id > 0 && request.request_id <= INT64_MAX && request.nonce > 0 &&
           request.nonce <= INT64_MAX && request.uid <= INT32_MAX && request.pid > 0 &&
           request.tgid > 0 && request.reserved == 0 &&
           memchr(request.comm, '\0', sizeof(request.comm)) != nullptr;
}

bool is_msud_process(pid_t pid) {
    const auto cmdline = read_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline) {
        return false;
    }
    const size_t first_end = cmdline->find('\0');
    if (first_end == std::string::npos) {
        return false;
    }
    const size_t arg1_begin = first_end + 1;
    const size_t arg1_end = cmdline->find('\0', arg1_begin);
    const size_t arg1_size =
        arg1_end == std::string::npos ? cmdline->size() - arg1_begin : arg1_end - arg1_begin;
    return std::string_view(*cmdline).substr(arg1_begin, arg1_size) == "msud";
}

bool is_root_msud_process(pid_t pid) {
    std::array<char, 64> proc_dir{};
    const int length = snprintf(proc_dir.data(), proc_dir.size(), "/proc/%d", pid);
    if (length <= 0 || static_cast<size_t>(length) >= proc_dir.size()) {
        return false;
    }
    struct stat status{};
    return stat(proc_dir.data(), &status) == 0 && status.st_uid == 0 && is_msud_process(pid);
}

bool read_process_start_time(pid_t pid, uint64_t* start_time) {
    if (start_time == nullptr) {
        return false;
    }
    const auto stat_line = read_file("/proc/" + std::to_string(pid) + "/stat");
    if (!stat_line) {
        return false;
    }
    const size_t comm_end = stat_line->rfind(')');
    if (comm_end == std::string::npos || comm_end + 1 >= stat_line->size()) {
        return false;
    }
    std::string_view fields = std::string_view(*stat_line).substr(comm_end + 1);
    for (int field = 3; field <= 22; ++field) {
        const std::string_view value = next_token(&fields);
        if (value.empty()) {
            return false;
        }
        if (field == 22) {
            return parse_uint64(value, start_time);
        }
    }
    return false;
}

bool read_process_executable(pid_t pid, struct stat* executable) {
    if (executable == nullptr) {
        return false;
    }
    std::array<char, 64> proc_exe{};
    const int length = snprintf(proc_exe.data(), proc_exe.size(), "/proc/%d/exe", pid);
    return length > 0 && static_cast<size_t>(length) < proc_exe.size() &&
           stat(proc_exe.data(), executable) == 0 && S_ISREG(executable->st_mode);
}

bool state_matches_process(const MsudState& state) {
    if (state.magic != kMsudStateMagic || state.version != kMsudStateVersion ||
        state.reserved != 0 || state.pid <= 0 || !is_root_msud_process(state.pid)) {
        return false;
    }
    uint64_t start_time = 0;
    struct stat executable{};
    return read_process_start_time(state.pid, &start_time) && start_time == state.start_time &&
           read_process_executable(state.pid, &executable) &&
           static_cast<uint64_t>(executable.st_dev) == state.exe_dev &&
           static_cast<uint64_t>(executable.st_ino) == state.exe_ino;
}

bool state_uses_current_daemon(const MsudState& state) {
    if (!state_matches_process(state)) {
        return false;
    }
    struct stat daemon{};
    return stat(DAEMON_PATH, &daemon) == 0 && S_ISREG(daemon.st_mode) &&
           static_cast<uint64_t>(daemon.st_dev) == state.exe_dev &&
           static_cast<uint64_t>(daemon.st_ino) == state.exe_ino;
}

bool valid_lock_fd(int fd) {
    struct stat status{};
    return fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == 0 &&
           status.st_gid == 0 && status.st_nlink == 1 && (status.st_mode & 0777) == 0600;
}

int acquire_lock(bool* already_running) {
    *already_running = false;
    const int fd = open(kMsudLockPath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 ||
        fchown(fd, 0, 0) != 0 || fchmod(fd, 0600) != 0) {
        close(fd);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        close(fd);
        *already_running = error == EWOULDBLOCK || error == EAGAIN;
        errno = error;
        return -1;
    }
    return fd;
}

bool write_msud_state(int lock_fd) {
    uint64_t start_time = 0;
    struct stat executable{};
    if (!read_process_start_time(getpid(), &start_time) ||
        !read_process_executable(getpid(), &executable)) {
        return false;
    }
    const MsudState state = {kMsudStateMagic,
                             kMsudStateVersion,
                             getpid(),
                             0,
                             start_time,
                             static_cast<uint64_t>(executable.st_dev),
                             static_cast<uint64_t>(executable.st_ino)};
    if (ftruncate(lock_fd, 0) != 0) {
        return false;
    }
    ssize_t count;
    do {
        count = pwrite(lock_fd, &state, sizeof(state), 0);
    } while (count < 0 && errno == EINTR);
    return count == sizeof(state) && fsync(lock_fd) == 0;
}

bool read_msud_state(MsudState* state) {
    if (state == nullptr) {
        return false;
    }
    const int fd = open(kMsudLockPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || !valid_lock_fd(fd)) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    ssize_t count;
    do {
        count = pread(fd, state, sizeof(*state), 0);
    } while (count < 0 && errno == EINTR);
    close(fd);
    return count == sizeof(*state) && state->magic == kMsudStateMagic &&
           state->version == kMsudStateVersion && state->reserved == 0;
}

bool lock_available() {
    const int fd = open(kMsudLockPath, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT;
    }
    if (!valid_lock_fd(fd)) {
        close(fd);
        return false;
    }
    const bool available = flock(fd, LOCK_EX | LOCK_NB) == 0;
    close(fd);
    return available;
}

bool prompt_consumer_ready() {
    MsudState state{};
    return read_msud_state(&state) && state_uses_current_daemon(state) && !lock_available();
}

bool wait_state_gone(const MsudState& state, int polls) {
    for (int attempt = 0; attempt < polls; ++attempt) {
        if (!state_matches_process(state)) {
            return true;
        }
        usleep(kStopPollIntervalUs);
    }
    return false;
}

bool stop_msud_process() {
    MsudState state{};
    if (!read_msud_state(&state) || !state_matches_process(state)) {
        return lock_available();
    }

#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int pid_fd = static_cast<int>(syscall(SYS_pidfd_open, state.pid, 0));
    if (pid_fd < 0 || !state_matches_process(state)) {
        if (pid_fd >= 0) {
            close(pid_fd);
        }
        return false;
    }
    if (syscall(SYS_pidfd_send_signal, pid_fd, SIGTERM, nullptr, 0) != 0 && errno != ESRCH) {
        close(pid_fd);
        return false;
    }
    if (!wait_state_gone(state, kStopGracePolls)) {
        if (syscall(SYS_pidfd_send_signal, pid_fd, SIGKILL, nullptr, 0) != 0 && errno != ESRCH) {
            close(pid_fd);
            return false;
        }
        if (!wait_state_gone(state, kStopKillPolls)) {
            close(pid_fd);
            return false;
        }
    }
    close(pid_fd);
#else
    if (kill(state.pid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
    }
    if (!wait_state_gone(state, kStopGracePolls)) {
        if (!state_matches_process(state) || (kill(state.pid, SIGKILL) != 0 && errno != ESRCH)) {
            return false;
        }
        if (!wait_state_gone(state, kStopKillPolls)) {
            return false;
        }
    }
#endif
    return lock_available();
}

void report_ready(int ready_fd, bool success) {
    if (ready_fd < 0) {
        return;
    }
    const uint8_t status = success ? 1 : 0;
    (void)write_all(ready_fd, &status, sizeof(status));
    close(ready_fd);
}

bool await_ready(int ready_fd) {
    struct pollfd descriptor{ready_fd, POLLIN | POLLHUP, 0};
    int poll_result;
    do {
        poll_result = poll(&descriptor, 1, kReadyTimeoutMs);
    } while (poll_result < 0 && errno == EINTR);
    uint8_t status = 0;
    ssize_t count;
    do {
        count = read(ready_fd, &status, sizeof(status));
    } while (count < 0 && errno == EINTR);
    close(ready_fd);
    return poll_result > 0 && count == sizeof(status) && status == 1;
}

void stop_spawn_process_group(pid_t process_group) {
    (void)kill(-process_group, SIGKILL);
    for (int attempt = 0; attempt < kStopKillPolls; ++attempt) {
        if (kill(-process_group, 0) != 0 && errno == ESRCH) {
            return;
        }
        usleep(kStopPollIntervalUs);
    }
}

int spawn_msud() {
    int ready_pipe[2];
    if (pipe2(ready_pipe, O_CLOEXEC) != 0) {
        return -1;
    }
    const pid_t launcher = fork();
    if (launcher < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return -1;
    }
    if (launcher == 0) {
        close(ready_pipe[0]);
        (void)setpgid(0, 0);
        switch_cgroups();

        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const pid_t daemon = fork();
        if (daemon < 0) {
            report_ready(ready_pipe[1], false);
            _exit(127);
        }
        if (daemon > 0) {
            close(ready_pipe[1]);
            _exit(0);
        }

        const int fd_flags = fcntl(ready_pipe[1], F_GETFD);
        if (fd_flags < 0 || fcntl(ready_pipe[1], F_SETFD, fd_flags & ~FD_CLOEXEC) != 0) {
            report_ready(ready_pipe[1], false);
            _exit(127);
        }
        std::array<char, 16> ready_argument{};
        if (snprintf(ready_argument.data(), ready_argument.size(), "%d", ready_pipe[1]) <= 0) {
            report_ready(ready_pipe[1], false);
            _exit(127);
        }
        char* const argv[] = {const_cast<char*>(DAEMON_PATH), const_cast<char*>("msud"),
                              const_cast<char*>("--ready-fd"), ready_argument.data(), nullptr};
        execv(DAEMON_PATH, argv);
        report_ready(ready_pipe[1], false);
        _exit(127);
    }

    close(ready_pipe[1]);
    int status = 0;
    while (waitpid(launcher, &status, 0) < 0 && errno == EINTR) {
    }
    const bool ready = await_ready(ready_pipe[0]);
    if (!ready) {
        stop_spawn_process_group(launcher);
        return prompt_consumer_ready() ? 0 : -1;
    }
    return 0;
}

int consume_prompt_requests(int prompt_fd) {
    for (;;) {
        struct pollfd descriptor{prompt_fd, POLLIN, 0};
        int poll_result;
        do {
            poll_result = poll(&descriptor, 1, -1);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            mlog("msud: prompt poll failed: %s", strerror(errno));
            return 1;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            mlog("msud: prompt fd closed (revents=0x%x)", descriptor.revents);
            return 1;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            ksu_su_prompt_request request{};
            ssize_t count;
            do {
                count = read(prompt_fd, &request, sizeof(request));
            } while (count < 0 && errno == EINTR);
            if (count != sizeof(request)) {
                mlog("msud: prompt read failed: count=%zd errno=%d", count, errno);
                return 1;
            }
            if (!valid_prompt_request(request)) {
                mlog("msud: rejected malformed prompt request");
                return 1;
            }
            if (!launch_prompt(request)) {
                mlog("msud: failed to launch Manager prompt for request %llu",
                     static_cast<unsigned long long>(request.request_id));
                const ksu_su_prompt_key key{request.request_id, request.nonce};
                if (ioctl(prompt_fd, KSU_IOCTL_CANCEL_SU_PROMPT, &key) < 0 && errno != ESTALE) {
                    return 1;
                }
            }
            continue;
        }
    }
}

}  // namespace

int run_msud(int ready_fd) {
    bool already_running = false;
    const int lock_fd = acquire_lock(&already_running);
    if (lock_fd < 0) {
        const bool ready = already_running && prompt_consumer_ready();
        report_ready(ready_fd, ready);
        return ready ? 0 : 1;
    }

    const int prompt_fd = get_su_prompt_fd();
    if (prompt_fd < 0) {
        mlog("msud: failed to acquire kernel prompt fd");
        report_ready(ready_fd, false);
        close(lock_fd);
        return 1;
    }
    if (!write_msud_state(lock_fd)) {
        mlog("msud: failed to publish daemon identity");
        report_ready(ready_fd, false);
        close(prompt_fd);
        close(lock_fd);
        return 1;
    }

    report_ready(ready_fd, true);
    mlog("msud: kernel prompt consumer ready");
    const int result = consume_prompt_requests(prompt_fd);
    close(prompt_fd);
    close(lock_fd);
    return result;
}

int ensure_msud_running_locked() {
    if (prompt_consumer_ready()) {
        return 0;
    }
    if (!lock_available() && !stop_msud_process()) {
        mlog("msud: refusing to replace an unidentified daemon owner");
        return -1;
    }
    return spawn_msud();
}

bool kill_msud_locked() {
    const bool stopped = stop_msud_process();
    if (!stopped) {
        mlog("msud: failed to stop prompt consumer");
    }
    return stopped;
}

void ensure_msud_running_if_enabled() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return;
    }
    const auto [value, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
    if (supported && value != 0 && ensure_msud_running_locked() != 0) {
        mlog("msud: failed to ensure prompt consumer");
    }
}

int apply_magisk_compat_now() {
    const SucompatTransitionLock transition;
    if (!transition.locked()) {
        return 1;
    }
    const auto [value, supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
    if (!supported) {
        return 1;
    }
    if (value != 0 && ensure_msud_running_locked() != 0) {
        return 1;
    }
    const int ret = set_feature(KSU_FEATURE_MAGISK_COMPAT, value);
    if (ret < 0) {
        const auto [current, current_supported] = get_feature(KSU_FEATURE_MAGISK_COMPAT);
        if (value != 0 && (!current_supported || current == 0)) {
            (void)kill_msud_locked();
        }
        return 1;
    }
    if (value == 0) {
        (void)kill_msud_locked();
    }
    return 0;
}

}  // namespace ksud
