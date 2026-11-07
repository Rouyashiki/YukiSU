#include "core/command.hpp"
#include "core/daemon.hpp"
#include "core/log.hpp"
#include "core/reboot_state.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "terminal.hpp"
#include "utils.hpp"

#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
fs::path root;
bool pending_hides = false;
bool controller_process = false;
bool recovery_race = false;
Clock::duration::rep race_deadline_ticks = 0;
int exit_request[2] = {-1, -1};
int exit_confirmed[2] = {-1, -1};

template <typename Predicate>
void wait_until(Predicate predicate, std::chrono::seconds timeout = 10s) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate() && Clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    assert(predicate());
}

void check_child(pid_t child) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

pid_t server_pid() {
    pid_t pid = -1;
    std::ifstream(kagami::runtime_pid_file()) >> pid;
    assert(pid > 0);
    return pid;
}

void wait_stopped(std::chrono::seconds timeout = 10s) {
    wait_until(
        [] {
            return !fs::exists(kagami::runtime_pid_file()) &&
                   !fs::exists(kagami::runtime_socket_file());
        },
        timeout);
    int status = 0;
    pid_t child;
    while ((child = waitpid(-1, &status, WNOHANG)) > 0)
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void call(const std::vector<std::string>& args) {
    assert(kagami::run_via_daemon(args) == 0);
    assert(kagami::last_daemon_request_completed());
}

std::set<std::string> calls(pid_t* owner = nullptr) {
    std::ifstream input(root / "calls");
    std::set<std::string> seen;
    std::string id;
    pid_t pid;
    while (input >> id >> pid) {
        assert(seen.insert(id).second);
        if (owner) {
            if (*owner > 0)
                assert(*owner == pid);
            *owner = pid;
        }
    }
    return seen;
}

void lifecycle_test() {
    call({"count", "first"});
    const auto first = server_pid();
    std::string name;
    std::ifstream("/proc/" + std::to_string(first) + "/comm") >> name;
    assert(name == "kagamid");
    wait_stopped();

    call({"count", "second"});
    assert(server_pid() != first);
    const int gate = open((root / "kagamid.requests.lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(gate >= 0 && flock(gate, LOCK_SH) == 0);
    const auto pinned = server_pid();
    std::this_thread::sleep_for(3500ms);
    assert(server_pid() == pinned);
    call({"delay", "long"});
    assert(server_pid() == pinned);
    close(gate);
    wait_stopped();

    assert(kagami::run_via_daemon({"crash", "once"}) != 0);
    call({"count", "after-crash"});
    wait_stopped();
    assert(calls() == std::set<std::string>({"first", "second", "long", "once", "after-crash"}));
}

void concurrent_test() {
    int barrier[2];
    assert(pipe2(barrier, O_CLOEXEC) == 0);
    std::vector<pid_t> clients;
    for (int index = 0; index < 12; ++index) {
        const auto child = fork();
        assert(child >= 0);
        if (child == 0) {
            close(barrier[1]);
            char start;
            assert(read(barrier[0], &start, 1) == 1);
            close(barrier[0]);
            call({"count", std::to_string(index)});
            _exit(0);
        }
        clients.push_back(child);
    }
    close(barrier[0]);
    for (std::size_t index = 0; index < clients.size(); ++index)
        assert(write(barrier[1], "1", 1) == 1);
    close(barrier[1]);
    for (const auto child : clients)
        check_child(child);
    pid_t owner = -1;
    assert(calls(&owner).size() == clients.size());
    assert(owner == server_pid());
    wait_stopped();
}

void recovery_test() {
    std::string error;
    assert(kagami::reboot::acquire(error));
    call({"count", "transaction"});
    const auto pid = server_pid();
    std::this_thread::sleep_for(3500ms);
    assert(server_pid() == pid);
    assert(kagami::reboot::record("complete", "completed"));
    kagami::reboot::release();
    wait_stopped();
    assert(!fs::exists(root / "recovered"));

    const auto coordinator = fork();
    assert(coordinator >= 0);
    if (coordinator == 0) {
        assert(kagami::reboot::acquire(error));
        assert(kagami::reboot::record("rebuild", "accepted"));
        call({"count", "abandoned"});
        _exit(0);
    }
    check_child(coordinator);
    wait_until([] { return fs::exists(root / "recovered"); });
    wait_stopped();
    assert(kagami::reboot::state().at("result").string_or("") == "failed");
    assert(!kagami::reboot::active());
}

void pending_test() {
    std::ofstream(root / "pending") << "saved legacy rule\n";
    const auto started = Clock::now();
    call({"count", "pending"});
    std::this_thread::sleep_for(3500ms);
    assert(server_pid() > 0);
    wait_stopped(35s);
    assert(Clock::now() - started >= 30s);
    assert(fs::exists(root / "pending"));
    call({"count", "retry"});
    fs::remove(root / "pending");
    call({"clear-pending", "restored"});
    wait_stopped();
}

void recovery_race_test() {
    assert(pipe2(exit_request, O_CLOEXEC) == 0);
    assert(pipe2(exit_confirmed, O_CLOEXEC) == 0);
    recovery_race = true;
    race_deadline_ticks = (Clock::now() + 3500ms).time_since_epoch().count();
    const auto coordinator = fork();
    assert(coordinator >= 0);
    if (coordinator == 0) {
        std::string error;
        assert(kagami::reboot::acquire(error));
        assert(kagami::reboot::record("rebuild", "accepted"));
        call({"count", "race"});
        char value;
        assert(read(exit_request[0], &value, 1) == 1);
        _exit(0);
    }
    check_child(coordinator);
    assert(write(exit_confirmed[1], "1", 1) == 1);
    wait_until([] { return fs::exists(root / "recovered"); });
    wait_stopped();
    assert(kagami::reboot::state().at("result").string_or("") == "failed");
    for (const auto fd : {exit_request[0], exit_request[1], exit_confirmed[0], exit_confirmed[1]})
        close(fd);
}
}  // namespace

extern "C" int real_fcntl(int fd, int command, ...) asm("__real_fcntl");
extern "C" int wrap_fcntl(int fd, int command, ...) asm("__wrap_fcntl");
extern "C" int wrap_fcntl(int fd, int command, ...) {
    assert(command == F_GETLK || command == F_SETLK);
    va_list args;
    va_start(args, command);
    auto* lock = va_arg(args, struct flock*);
    va_end(args);
    const int result = real_fcntl(fd, command, lock);
    if (result == 0 && command == F_GETLK && lock->l_type != F_UNLCK && controller_process &&
        recovery_race && Clock::now().time_since_epoch().count() >= race_deadline_ticks) {
        recovery_race = false;
        assert(write(exit_request[1], "1", 1) == 1);
        char value;
        assert(read(exit_confirmed[0], &value, 1) == 1);
    }
    return result;
}

namespace ksud {
ProcessDaemonizeResult daemonize_process(bool use_init_pgrp) {
    assert(!use_init_pgrp);
    const auto child = fork();
    assert(child >= 0);
    if (child > 0) {
        check_child(child);
        return ProcessDaemonizeResult::Parent;
    }
    assert(setsid() >= 0);
    const auto daemon = fork();
    assert(daemon >= 0);
    if (daemon > 0)
        _exit(0);
    controller_process = true;
    return ProcessDaemonizeResult::Daemon;
}

bool write_file_atomic(const fs::path& path, const std::string& content) {
    const auto temporary = path.string() + ".tmp";
    std::ofstream(temporary) << content;
    fs::rename(temporary, path);
    return true;
}

bool recover_soft_reboot(const std::string& phase, [[maybe_unused]] std::string& error) {
    assert(phase == "rebuild");
    assert(kagami::reboot::authorized());
    std::ofstream(root / "recovered") << phase << '\n';
    return true;
}

namespace terminal {
int error(std::string_view text, [[maybe_unused]] std::string_view hint) {
    std::cerr << text << '\n';
    return 1;
}
void diagnostics(std::string_view text) {
    std::cerr << text << '\n';
}
}  // namespace terminal
}  // namespace ksud

namespace kagami {
fs::path runtime_data_dir() {
    return root;
}
fs::path runtime_config_file() {
    return root / "config.json";
}
fs::path runtime_log_file() {
    return root / "kagami.log";
}
fs::path runtime_socket_file() {
    return root / "kagamid.sock";
}
fs::path runtime_pid_file() {
    return root / "kagamid.pid";
}
fs::path runtime_daemon_lock_file() {
    return root / "kagamid.lock";
}
std::string runtime_boot_id() {
    return "daemon-test-boot";
}
bool prepare_runtime([[maybe_unused]] std::string& error) {
    fs::create_directories(root / "run");
    return true;
}
bool read_config_file(Config& config, [[maybe_unused]] std::string& error,
                      [[maybe_unused]] MissingConfig missing) {
    config.kasumi_enabled = fs::exists(root / "pending");
    return true;
}

CommandResult run_command_capture(const std::vector<std::string>& args,
                                  [[maybe_unused]] CommandCapture& capture) {
    assert(args.size() == 2);
    std::ofstream(root / "calls", std::ios::app) << args[1] << ' ' << getpid() << '\n';
    if (args[0] == "delay")
        std::this_thread::sleep_for(3500ms);
    if (args[0] == "crash")
        _exit(0);
    if (args[0] == "clear-pending")
        pending_hides = false;
    return {};
}

namespace logging {
void set_debug_enabled([[maybe_unused]] bool value) noexcept {}
void write([[maybe_unused]] Level level, [[maybe_unused]] const std::string& component,
           [[maybe_unused]] const std::string& message) noexcept {}
bool prepare_boot_log([[maybe_unused]] std::string& error,
                      [[maybe_unused]] const std::string& boot_id) {
    return true;
}
}  // namespace logging

namespace mount::kasumi {
bool restore_persisted_hide_rules([[maybe_unused]] std::string& error,
                                  [[maybe_unused]] bool before_start) {
    pending_hides = fs::exists(root / "pending");
    return !pending_hides;
}
bool has_pending_hide_rules() {
    return pending_hides;
}
void retry_pending_hide_rules() {
    pending_hides = fs::exists(root / "pending");
}
}  // namespace mount::kasumi
}  // namespace kagami

int main(int argc, char** argv) {
    assert(argc == 2);
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
    char directory[] = "/tmp/yukisu-kagami-daemon-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    const std::string test = argv[1];
    if (test == "lifecycle")
        lifecycle_test();
    else if (test == "concurrent")
        concurrent_test();
    else if (test == "recovery")
        recovery_test();
    else if (test == "pending")
        pending_test();
    else if (test == "recovery-race")
        recovery_race_test();
    else
        assert(false);
    fs::remove_all(root);
    std::cout << "kagami " << test << " passed\n";
}
