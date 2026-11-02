#include "soft_reboot_waiter.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

namespace ksud {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::chrono::milliseconds remaining_time(Clock::time_point deadline) {
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
}

std::optional<pid_t> parse_pid(std::string_view text) {
    text = trim_view(text);
    if (text.empty())
        return std::nullopt;
    pid_t pid = -1;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (error != std::errc{} || end != text.data() + text.size() || pid <= 1)
        return std::nullopt;
    return pid;
}

bool valid_service_name(std::string_view name) {
    return !name.empty() && name.size() <= 512 &&
           std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch <= ' '; });
}

bool parse_service_list(std::string_view output, std::map<std::string, pid_t>& services,
                        std::string_view& dumps) {
    services.clear();
    constexpr std::string_view header = "Currently running services:";
    const auto first_newline = output.find('\n');
    if (first_newline == std::string_view::npos ||
        trim_view(output.substr(0, first_newline)) != header)
        return false;
    output.remove_prefix(first_newline + 1);
    while (!output.empty()) {
        const auto newline = output.find('\n');
        const auto line = output.substr(0, newline);
        if (!trim_view(line).empty()) {
            if (!starts_with(line, "  "))
                break;
            const auto name = trim_view(line);
            if (!valid_service_name(name) || !services.emplace(std::string(name), -1).second)
                return false;
        }
        output.remove_prefix(newline == std::string_view::npos ? output.size() : newline + 1);
    }
    dumps = output;
    return true;
}

bool collect_services(std::string_view output, pid_t server_pid,
                      std::vector<std::string>& tracked) {
    tracked.clear();
    std::map<std::string, pid_t> services;
    std::string_view dumps;
    if (!parse_service_list(output, services, dumps) || services.empty())
        return false;
    auto pending = services.end();
    bool valid = true;
    for_each_line(dumps, [&](std::string_view line) {
        line = trim_view(line);
        if (line.empty())
            return true;
        constexpr std::string_view prefix = "DUMP OF SERVICE ";
        if (pending != services.end()) {
            const auto pid = parse_pid(line);
            if (!pid) {
                valid = false;
                return false;
            }
            pending->second = *pid;
            pending = services.end();
        } else if (starts_with(line, prefix) && line.back() == ':') {
            const auto name = line.substr(prefix.size(), line.size() - prefix.size() - 1);
            pending = services.find(std::string(name));
            if (pending == services.end() || pending->second != -1) {
                valid = false;
                return false;
            }
        } else if (!starts_with(line, "---------")) {
            valid = false;
            return false;
        }
        return true;
    });
    const auto activity = services.find("activity");
    if (!valid || pending != services.end() || activity == services.end() ||
        activity->second != server_pid)
        return false;
    for (const auto& [name, pid] : services) {
        if (pid <= 1) {
            tracked.clear();
            return false;
        }
        if (pid == server_pid)
            tracked.push_back(name);
    }
    return true;
}

bool wait_for_process_exit(pid_t pid, int pidfd, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        const auto remaining = remaining_time(deadline);
        if (remaining.count() <= 0)
            break;
        if (pidfd >= 0) {
            pollfd descriptor{pidfd, POLLIN, 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
            if (ready > 0)
                return (descriptor.revents & POLLIN) != 0;
            if (ready < 0 && errno == EINTR)
                continue;
            return false;
        }
        if (kill(pid, 0) < 0 && errno == ESRCH)
            return true;
        std::this_thread::sleep_for(std::min(remaining_time(deadline), 200ms));
    }
    return false;
}
}  // namespace

SoftRebootWaiter::~SoftRebootWaiter() {
    if (server_pidfd_ >= 0)
        close(server_pidfd_);
    for (const auto& [pid, fd] : zygotes_) {
        (void)pid;
        if (fd >= 0)
            close(fd);
    }
}

bool SoftRebootWaiter::prepare() {
    for (const auto& [pid, fd] : zygotes_) {
        (void)pid;
        if (fd >= 0)
            close(fd);
    }
    zygotes_.clear();
    if (server_pidfd_ >= 0)
        close(server_pidfd_);
    server_pidfd_ = -1;
    server_pid_ = -1;
    services_.clear();
    const auto deadline = Clock::now() + 2s;
    const auto process =
        exec_command({"/system/bin/pidof", "-s", "system_server"}, remaining_time(deadline));
    const auto pid = process.exit_code == 0 ? parse_pid(process.stdout_str) : std::nullopt;
    if (!pid) {
        LOGW("Could not identify system_server before stop");
        return false;
    }
    server_pid_ = *pid;
#if defined(SYS_pidfd_open)
    server_pidfd_ = static_cast<int>(syscall(SYS_pidfd_open, server_pid_, 0));
#endif

    if (Clock::now() >= deadline)
        return false;
    const auto zygotes =
        exec_command({"/system/bin/pidof", "zygote", "zygote64"}, remaining_time(deadline));
    std::istringstream pids(zygotes.stdout_str);
    std::string value;
    while (pids >> value) {
        const auto zygote = parse_pid(value);
        if (!zygote)
            return false;
        int fd = -1;
#if defined(SYS_pidfd_open)
        fd = static_cast<int>(syscall(SYS_pidfd_open, *zygote, 0));
#endif
        zygotes_.emplace_back(*zygote, fd);
    }
    if (zygotes.exit_code != 0 || zygotes_.empty() || Clock::now() >= deadline) {
        LOGW("Could not identify zygote before stop");
        return false;
    }

    // dumpsys resolves each service through checkService and queries its Binder debug PID.
    const auto snapshot =
        exec_command({"/system/bin/dumpsys", "-T", "250", "--pid"}, remaining_time(deadline));
    if (snapshot.exit_code != 0 || !snapshot.stderr_str.empty() ||
        !collect_services(snapshot.stdout_str, server_pid_, services_)) {
        LOGW("Binder snapshot unavailable (exit=%d errno=%d); using system_server exit wait",
             snapshot.exit_code, snapshot.error_number);
        return true;
    }
    LOGI("Soft reboot monitor ready: system_server=%d, %zu Binder services", server_pid_,
         services_.size());
    return true;
}

bool SoftRebootWaiter::wait() {
    if (server_pid_ <= 1)
        return false;
    const auto deadline = Clock::now() + 5s;
    if (!wait_for_process_exit(server_pid_, server_pidfd_, deadline)) {
        LOGW("Could not confirm system_server exit before soft reboot timeout");
        return false;
    }
    for (const auto& [pid, fd] : zygotes_) {
        if (!wait_for_process_exit(pid, fd, deadline)) {
            LOGW("Could not confirm zygote %d exit", pid);
            return false;
        }
    }
    LOGI("system_server exited after stop");
    if (services_.empty())
        return true;
    while (Clock::now() < deadline) {
        const auto running = exec_command({"/system/bin/dumpsys", "-l"}, remaining_time(deadline));
        std::map<std::string, pid_t> services;
        std::string_view tail;
        if (running.exit_code != 0 || !running.stderr_str.empty() ||
            !parse_service_list(running.stdout_str, services, tail) || !trim_view(tail).empty()) {
            LOGW("Binder cleanup query failed (exit=%d errno=%d)", running.exit_code,
                 running.error_number);
            return false;
        }
        if (std::none_of(services_.begin(), services_.end(),
                         [&](const auto& name) { return services.find(name) != services.end(); })) {
            LOGI("system_server Binder services removed from servicemanager");
            return true;
        }
        std::this_thread::sleep_for(std::min(remaining_time(deadline), 200ms));
    }
    LOGW("Binder cleanup timed out");
    return false;
}

}  // namespace ksud
