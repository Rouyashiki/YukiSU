#include "yukizygisk_daemon.hpp"

#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "uapi/yukizygisk.h"
#include "userspace/zygisk/daemon_health_client.hpp"
#include "userspace/zygisk/daemon_state.hpp"
#include "utils.hpp"
#include "yukizygisk_snapshot.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ksud {
namespace {

namespace health = yukizygisk::health;
using Clock = std::chrono::steady_clock;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0)
            close(fd_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;
    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

int remaining_ms(Clock::time_point deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::max<int64_t>(0, remaining));
}

const char* availability_name(health::Availability value) {
    switch (value) {
    case health::Availability::Available:
        return "available";
    case health::Availability::Missing:
        return "missing";
    case health::Availability::Unresponsive:
        return "unresponsive";
    case health::Availability::Unsupported:
        return "unsupported";
    case health::Availability::IdentityError:
        return "identity_error";
    case health::Availability::Error:
        return "error";
    }
    return "error";
}

const char* reader_name(health::ReaderState state) {
    switch (state) {
    case health::ReaderState::Unsupported:
        return "unsupported";
    case health::ReaderState::Active:
        return "active";
    case health::ReaderState::Retrying:
        return "retrying";
    case health::ReaderState::Unknown:
        return "unknown";
    }
    return "unknown";
}

json::Value number(uint32_t value) {
    return {static_cast<double>(value)};
}

bool needs_daemon32() {
    if (access("/system_ext/bin/tango_translator", X_OK) == 0 &&
        access("/system/bin/app_process32", X_OK) == 0)
        return true;
    const auto zygote = getprop("ro.zygote");
    const bool zygote32 = zygote ? zygote->find("32") != std::string::npos
                                 : access("/system/bin/app_process32", X_OK) == 0;
    return zygote32 || yukizygisk_has_native_abi32_target();
}

int lock_operation(int directory) {
    const int fd = openat(directory, "ensure.lock",
                          O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != 0 ||
        status.st_nlink != 1 || (status.st_mode & 0777) != 0600) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

bool readable_before(int fd, Clock::time_point deadline) {
    pollfd descriptor{fd, POLLIN, 0};
    for (;;) {
        const int left = remaining_ms(deadline);
        if (left == 0)
            return false;
        const int ready = poll(&descriptor, 1, left);
        if (ready < 0 && errno == EINTR)
            continue;
        return ready > 0 && (descriptor.revents & POLLIN) != 0;
    }
}

void terminate_owned(int pidfd) {
    if (pidfd >= 0)
        (void)syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
}

bool send_owned_identity(int channel, pid_t pid, int pidfd) {
    const int32_t identity = pid;
    iovec vector{const_cast<int32_t*>(&identity), sizeof(identity)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &pidfd, sizeof(pidfd));
    return sendmsg(channel, &message, MSG_NOSIGNAL) == sizeof(identity);
}

int receive_owned_identity(int channel, pid_t* pid) {
    int32_t identity = -1;
    iovec vector{&identity, sizeof(identity)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = recvmsg(channel, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    const auto* header = CMSG_FIRSTHDR(&message);
    int pidfd = -1;
    if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len == CMSG_LEN(sizeof(int)))
        memcpy(&pidfd, CMSG_DATA(header), sizeof(pidfd));
    if (received != sizeof(identity) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        identity <= 1 || pidfd < 0) {
        if (pidfd >= 0)
            close(pidfd);
        return -1;
    }
    *pid = identity;
    return pidfd;
}

int spawn_daemon(const char* path, Clock::time_point operation_deadline, int operation_lock,
                 pid_t* spawned_pid = nullptr) {
    const auto deadline = std::min(operation_deadline, Clock::now() + std::chrono::seconds(5));
    int ready_pipe[2] = {-1, -1};
    if (pipe2(ready_pipe, O_CLOEXEC) != 0)
        return 2;
    int identity_socket[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, identity_socket) != 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return 2;
    }
    const pid_t launcher = fork();
    if (launcher < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        close(identity_socket[0]);
        close(identity_socket[1]);
        return 2;
    }
    if (launcher == 0) {
        close(ready_pipe[0]);
        close(identity_socket[0]);
        close(operation_lock);
        char gate = 0;
        if (recv(identity_socket[1], &gate, 1, 0) != 1 || gate != 'F')
            _exit(127);
        if (setpgid(0, 0) != 0)
            _exit(127);
        (void)signal(SIGCHLD, SIG_DFL);
        switch_cgroups();
        if (!reset_stdio_to_devnull())
            _exit(127);
        const pid_t child = fork();
        if (child < 0)
            _exit(127);
        if (child > 0) {
            const int child_fd = static_cast<int>(syscall(SYS_pidfd_open, child, 0));
            if (child_fd < 0 || !send_owned_identity(identity_socket[1], child, child_fd))
                _exit(127);
            _exit(0);
        }
        if (recv(identity_socket[1], &gate, 1, 0) != 1 || gate != 'G')
            _exit(127);
        close(identity_socket[1]);
        if (fcntl(ready_pipe[1], F_SETFD, 0) != 0)
            _exit(127);
        char descriptor[24];
        (void)snprintf(descriptor, sizeof(descriptor), "%d", ready_pipe[1]);
        if (setenv("YUKIZYGISK_READY_FD", descriptor, 1) != 0)
            _exit(127);
        char* const arguments[] = {const_cast<char*>(path), nullptr};
        execv(path, arguments);
        const char failed = health::kFailed;
        (void)write(ready_pipe[1], &failed, 1);
        _exit(127);
    }
    close(ready_pipe[1]);
    close(identity_socket[1]);
    const FileDescriptor ready(ready_pipe[0]);
    const FileDescriptor channel(identity_socket[0]);
    const FileDescriptor launcher_fd(static_cast<int>(syscall(SYS_pidfd_open, launcher, 0)));
    if (launcher_fd.get() < 0)
        return 2;
    int target_fd = -1;
    auto finish = [&](int result) {
        if (result != 0) {
            terminate_owned(target_fd);
            terminate_owned(launcher_fd.get());
        }
        (void)readable_before(launcher_fd.get(), operation_deadline);
        (void)waitpid(launcher, nullptr, WNOHANG);
        return result;
    };
    const char start = 'F';
    if (send(channel.get(), &start, 1, MSG_NOSIGNAL) != 1 ||
        !readable_before(channel.get(), deadline))
        return finish(2);
    pid_t child = -1;
    target_fd = receive_owned_identity(channel.get(), &child);
    const FileDescriptor target(target_fd);
    if (target_fd < 0 || !readable_before(launcher_fd.get(), deadline))
        return finish(2);
    const char execute = 'G';
    if (send(channel.get(), &execute, 1, MSG_NOSIGNAL) != 1 ||
        !readable_before(ready.get(), deadline))
        return finish(2);
    char response = health::kFailed;
    const ssize_t received = read(ready.get(), &response, 1);
    if (received == 1 && response == health::kReady) {
        if (spawned_pid)
            *spawned_pid = child;
        return finish(0);
    }
    return finish(received == 1 && response == health::kRebootRequired ? 3 : 2);
}

}  // namespace

json::Value yukizygisk_kernel_health(uint32_t capabilities) {
    json::Value result = json::Value::object();
    result["supported"] = (capabilities & YZ_RUNTIME_CAP_HEALTH) != 0;
    if ((capabilities & YZ_RUNTIME_CAP_HEALTH) == 0) {
        result["state"] = "unsupported";
        return result;
    }
    yz_health_query_cmd query{};
    query.version = YZ_HEALTH_VERSION;
    query.size = sizeof(query);
    if (ksuctl(KSU_IOCTL_YZ_GET_HEALTH, &query) != 0) {
        const int error = errno;
        if (error == ENOTTY || error == EOPNOTSUPP)
            result["supported"] = false;
        result["state"] = error == ENOTTY || error == EOPNOTSUPP ? "unsupported" : "error";
        result["error"] = error;
        return result;
    }
    result["state"] = "available";
    result["sample_begin_boottime_ns"] = std::to_string(query.sample_begin_boottime);
    result["sample_end_boottime_ns"] = std::to_string(query.sample_end_boottime);
    auto& history = result["history"];
    history["epoch"] = std::to_string(query.history.epoch);
    history["oldest_sequence"] = std::to_string(query.history.oldest_sequence);
    history["newest_sequence"] = std::to_string(query.history.newest_sequence);
    history["coverage_generation"] = std::to_string(query.history.coverage_generation);
    history["count"] = number(query.history.count);
    history["observer_active"] = query.history.observer_active != 0;
    auto& policy = result["policy"];
    policy["enabled"] = query.policy.enabled != 0;
#define YZ_HEALTH_COUNT(field) policy[#field] = number(query.policy.field)
    YZ_HEALTH_COUNT(states_current);
    YZ_HEALTH_COUNT(states_peak);
    YZ_HEALTH_COUNT(preparing);
    YZ_HEALTH_COUNT(native_active);
    YZ_HEALTH_COUNT(module_groups_active);
    YZ_HEALTH_COUNT(holders_current);
    YZ_HEALTH_COUNT(holders_peak);
    YZ_HEALTH_COUNT(retired_current);
    YZ_HEALTH_COUNT(restore_inflight);
    YZ_HEALTH_COUNT(retry_waiting);
    YZ_HEALTH_COUNT(last_restore_kind);
#undef YZ_HEALTH_COUNT
#define YZ_HEALTH_TOTAL(field) policy[#field] = std::to_string(query.policy.field)
    YZ_HEALTH_TOTAL(oldest_retired_boottime);
    YZ_HEALTH_TOTAL(last_restore_boottime);
    YZ_HEALTH_TOTAL(restore_attempts);
    YZ_HEALTH_TOTAL(restore_failures);
    YZ_HEALTH_TOTAL(restore_successes);
#undef YZ_HEALTH_TOTAL
    policy["last_restore_errno"] = query.policy.last_restore_errno;
    auto& cleanup = result["cleanup"];
    cleanup["queued_owners"] = number(query.cleanup.queued_owners);
    cleanup["inflight_owners"] = number(query.cleanup.inflight_owners);
    cleanup["queue_peak"] = number(query.cleanup.queue_peak);
    cleanup["reconcile_pending"] = query.cleanup.reconcile_pending != 0;
#define YZ_HEALTH_TOTAL(field) cleanup[#field] = std::to_string(query.cleanup.field)
    YZ_HEALTH_TOTAL(queue_enqueued);
    YZ_HEALTH_TOTAL(queue_overflows);
    YZ_HEALTH_TOTAL(owner_cleanup_calls);
    YZ_HEALTH_TOTAL(owner_index_visits);
    YZ_HEALTH_TOTAL(watch_index_visits);
    YZ_HEALTH_TOTAL(fullscan_overflow);
    YZ_HEALTH_TOTAL(fullscan_enable);
    YZ_HEALTH_TOTAL(fullscan_missing_owner);
    YZ_HEALTH_TOTAL(fullscan_disable);
    YZ_HEALTH_TOTAL(fullscan_entries);
    YZ_HEALTH_TOTAL(alive_checks_publish);
    YZ_HEALTH_TOTAL(alive_checks_scan);
    YZ_HEALTH_TOTAL(exit_worker_runs);
    YZ_HEALTH_TOTAL(retry_worker_runs);
#undef YZ_HEALTH_TOTAL
    return result;
}

json::Value yukizygisk_daemon_health() {
    const auto replies = health::query_all();
    json::Value result = json::Value::array();
    for (size_t index = 0; index < replies.size(); ++index) {
        const auto& reply = replies[index];
        json::Value item = json::Value::object();
        item["abi_id"] = number(static_cast<uint32_t>(index + 1));
        item["abi"] = index == 0 ? "armeabi-v7a" : "arm64-v8a";
        item["source"] = "live";
        item["state"] = availability_name(reply.availability);
        item["error"] = reply.error;
        if (reply.availability == health::Availability::Available) {
            const auto& snapshot = reply.snapshot;
            const char* backend = nullptr;
            if (reply.execution_backend == health::ExecutionBackend::Tango)
                backend = "tango";
            else
                backend = reply.execution_backend == health::ExecutionBackend::Native ? "native"
                                                                                      : "unknown";
            item["execution_backend"] = backend;
            item["pid"] = number(snapshot.pid);
            item["start_ticks"] = std::to_string(snapshot.start_ticks);
            item["sampled_boottime_ns"] = std::to_string(snapshot.sampled_boottime_ns);
            item["ready"] = (snapshot.flags & health::Ready) != 0;
            item["history_available"] = (snapshot.flags & health::HistoryAvailable) != 0;
            item["persistence_pending"] = (snapshot.flags & health::PersistencePending) != 0;
            item["committed_valid"] = (snapshot.flags & health::CommittedValid) != 0;
            item["catalog_frozen"] = (snapshot.flags & health::CatalogFrozen) != 0;
            item["reboot_required"] = (snapshot.flags & health::RebootRequired) != 0;
            item["reader_state"] = reader_name(snapshot.reader_state);
            item["read_error"] = snapshot.read_error;
            item["save_error"] = snapshot.save_error;
            item["catalog_error"] = snapshot.catalog_error;
#define YZ_HEALTH_TOTAL(field) item[#field] = std::to_string(snapshot.field)
            YZ_HEALTH_TOTAL(consumed_epoch);
            YZ_HEALTH_TOTAL(consumed_cursor);
            YZ_HEALTH_TOTAL(newest_observed);
            YZ_HEALTH_TOTAL(committed_epoch);
            YZ_HEALTH_TOTAL(committed_cursor);
            YZ_HEALTH_TOTAL(read_retry_at_ns);
            YZ_HEALTH_TOTAL(read_failures);
            YZ_HEALTH_TOTAL(save_retry_at_ns);
            YZ_HEALTH_TOTAL(save_failures);
            YZ_HEALTH_TOTAL(last_saved_boottime_ns);
            YZ_HEALTH_TOTAL(last_read_boottime_ns);
            YZ_HEALTH_TOTAL(poll_returns);
            YZ_HEALTH_TOTAL(poll_timeouts);
            YZ_HEALTH_TOTAL(history_drains);
#undef YZ_HEALTH_TOTAL
            item["bound_sessions"] = number(snapshot.bound_sessions);
            item["unbound_sessions"] = number(snapshot.unbound_sessions);
            item["companions"] = number(snapshot.companions);
            item["starting_companions"] = number(snapshot.starting_companions);
            item["terminating_companions"] = number(snapshot.terminating_companions);
            item["catalog_sha256"] =
                std::string(snapshot.catalog_sha256, sizeof(snapshot.catalog_sha256));
        }
        result.push_back(item);
    }
    return result;
}

int ensure_yukizygisk_daemons(uint32_t abi) {
    if (geteuid() != 0 || abi > 2) {
        (void)fprintf(stderr, "yzctl: root and a valid ABI are required\n");
        return 1;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    if (!switch_mnt_ns(1)) {
        (void)fprintf(stderr, "yzctl: cannot enter init mount namespace\n");
        return 2;
    }
    const auto [enabled, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    yz_safemode_status_cmd mode{};
    if (!supported || enabled == 0 || is_safe_mode() ||
        ksuctl(KSU_IOCTL_YZ_GET_SAFEMODE, &mode) != 0 || mode.active != 0) {
        (void)fprintf(stderr, "yzctl: daemon start unavailable while disabled or in safe mode\n");
        return 2;
    }
    const FileDescriptor directory(health::open_recovery_directory());
    if (directory.get() < 0) {
        (void)fprintf(stderr, "yzctl: recovery directory unavailable: %s\n", strerror(errno));
        return 2;
    }
    const FileDescriptor operation(lock_operation(directory.get()));
    if (operation.get() < 0) {
        (void)fprintf(stderr, "yzctl: daemon operation busy or inaccessible: %s\n",
                      strerror(errno));
        return 2;
    }
    const bool require32 = needs_daemon32();
    if (abi == 1 && !require32) {
        (void)fprintf(stderr, "yzctl: 32-bit daemon is not required by this configuration\n");
        return 2;
    }
    const auto initial = health::query_all(std::min(1000, remaining_ms(deadline)));
    int result = 0;
    for (const uint32_t selected : {2U, 1U}) {
        if ((abi != 0 && abi != selected) || (selected == 1 && !require32))
            continue;
        const char* path = selected == 2 ? ZYGISKD64_PATH : ZYGISKD32_PATH;
        const auto& current = initial[selected - 1];
        if (current.availability == health::Availability::Available) {
            if ((current.snapshot.flags & health::RebootRequired) != 0) {
                (void)fprintf(stderr, "yzctl: daemon%u module catalog changed; reboot_required\n",
                              selected == 2 ? 64U : 32U);
                result = 3;
            } else if (current.snapshot.catalog_error != 0) {
                (void)fprintf(stderr, "yzctl: daemon%u catalog unavailable: %s\n",
                              selected == 2 ? 64U : 32U, strerror(current.snapshot.catalog_error));
                result = std::max(result, 2);
            } else if ((current.snapshot.flags & health::Ready) == 0) {
                (void)fprintf(stderr, "yzctl: daemon%u is still initializing\n",
                              selected == 2 ? 64U : 32U);
                result = std::max(result, 2);
            } else {
                printf("daemon%u already running (pid=%u)\n", selected == 2 ? 64U : 32U,
                       current.snapshot.pid);
            }
            continue;
        }
        if (current.availability != health::Availability::Missing) {
            (void)fprintf(stderr, "yzctl: daemon%u %s; existing instance left running\n",
                          selected == 2 ? 64U : 32U, availability_name(current.availability));
            result = std::max(result, 2);
            continue;
        }
        if (remaining_ms(deadline) == 0 || access(path, X_OK) != 0) {
            (void)fprintf(stderr, "yzctl: daemon%u executable unavailable or deadline expired\n",
                          selected == 2 ? 64U : 32U);
            result = std::max(result, 2);
            continue;
        }
        const int lifetime = health::lock_daemon(selected);
        if (lifetime < 0) {
            (void)fprintf(stderr, "yzctl: daemon%u lifecycle lock busy or inaccessible\n",
                          selected == 2 ? 64U : 32U);
            result = std::max(result, 2);
            continue;
        }
        close(lifetime);
        pid_t spawned = -1;
        const int launched = spawn_daemon(path, deadline, operation.get(), &spawned);
        if (launched != 0) {
            (void)fprintf(stderr, "yzctl: daemon%u %s\n", selected == 2 ? 64U : 32U,
                          launched == 3 ? "reboot_required" : "failed readiness");
            result = std::max(result, launched);
            continue;
        }
        const auto confirmed = health::query(selected, std::min(1000, remaining_ms(deadline)));
        if (confirmed.availability != health::Availability::Available ||
            confirmed.snapshot.pid != static_cast<uint32_t>(spawned) ||
            (confirmed.snapshot.flags & health::Ready) == 0) {
            (void)fprintf(stderr, "yzctl: daemon%u started but health could not be confirmed\n",
                          selected == 2 ? 64U : 32U);
            result = std::max(result, 2);
            continue;
        }
        if ((confirmed.snapshot.flags & health::RebootRequired) != 0) {
            (void)fprintf(stderr, "yzctl: daemon%u started but requires reboot\n",
                          selected == 2 ? 64U : 32U);
            result = 3;
        } else if (confirmed.snapshot.catalog_error != 0) {
            (void)fprintf(stderr, "yzctl: daemon%u started but catalog is unavailable: %s\n",
                          selected == 2 ? 64U : 32U, strerror(confirmed.snapshot.catalog_error));
            result = std::max(result, 2);
        } else {
            printf("daemon%u ready (pid=%u)\n", selected == 2 ? 64U : 32U, confirmed.snapshot.pid);
        }
    }
    return result;
}

}  // namespace ksud
