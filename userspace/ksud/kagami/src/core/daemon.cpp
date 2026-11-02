#include "core/daemon.hpp"
#include "core/socket_output.hpp"
#include "init_event.hpp"
#include "terminal.hpp"
#include "utils.hpp"

#include "core/command.hpp"
#include "core/json.hpp"
#include "core/log.hpp"
#include "core/reboot_state.hpp"
#include "core/runtime.hpp"
#include "kagami/config.hpp"
#include "kagami/kasumi_client.hpp"
#include "mount/kasumi.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace kagami {

namespace fs = std::filesystem;

namespace {
int print_status_json();
bool request_completed = false;

void append_log(const std::string& message, logging::Level level = logging::Level::Info) {
    logging::write(level, "daemon", message);
}

std::string join_request(const std::vector<std::string>& args, std::size_t start) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = start; i < args.size(); ++i) {
        if (i > start) {
            out << ',';
        }
        out << json_quote(args[i]);
    }
    out << "]\n";
    return out.str();
}

std::vector<std::string> split_request(const std::string& request) {
    std::vector<std::string> args;
    JsonValue root;
    std::string error;
    if (!parse_json(request, root, error) || !root.is_array()) {
        return args;
    }
    args.reserve(root.a.size());
    for (const auto& item : root.a) {
        if (!item.is_string() || item.s.find('\0') != std::string::npos) {
            args.clear();
            return args;
        }
        args.push_back(item.s);
    }
    return args;
}

bool send_response(int fd, bool ok, int exit_code, int error_number, std::string_view output,
                   std::string_view error) {
    char buffer[4096];
    return daemon_protocol::send_response(fd, ok, exit_code, error_number, output, error, buffer,
                                          sizeof(buffer));
}

std::string status_json(bool running) {
    const auto pid_text = [&]() -> std::string {
        std::ifstream in(runtime_pid_file());
        std::string line;
        return std::getline(in, line) ? line : "";
    }();

    std::ostringstream out;
    out << "{"
        << "\"soft_reboot_protocol\":1,"
        << "\"running\":" << (running ? "true" : "false") << ","
        << "\"data_dir\":" << json_quote(runtime_data_dir().string()) << ","
        << "\"config_file\":" << json_quote(runtime_config_file().string()) << ","
        << "\"socket\":" << json_quote(runtime_socket_file().string()) << ","
        << "\"pid_file\":" << json_quote(runtime_pid_file().string()) << ","
        << "\"pid\":" << json_quote(pid_text) << ","
        << "\"log_file\":" << json_quote(runtime_log_file().string()) << "}\n";
    return out.str();
}

bool write_all(int fd, const std::string& data) {
    const char* ptr = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t written = send(fd, ptr, left, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        ptr += written;
        left -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(int fd, std::size_t limit, std::string& data) {
    data.clear();
    char buffer[1024];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            data.clear();
            return false;
        }
        if (count == 0)
            return true;
        const auto size = static_cast<std::size_t>(count);
        if (size > limit - data.size()) {
            data.clear();
            errno = EMSGSIZE;
            return false;
        }
        data.append(buffer, size);
        if (std::memchr(buffer, '\n', size))
            return true;
    }
}

int open_client_socket(std::string& error, int timeout_seconds) {
    const auto path = runtime_socket_file().string();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        error = "socket path is too long: " + path;
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }

    timeval timeout = {};
    timeout.tv_sec = timeout_seconds;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int saved_errno = errno;
        error = std::strerror(saved_errno);
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int send_request(const std::vector<std::string>& args, std::size_t start, std::string& response,
                 std::string& error, int timeout_seconds = 0) {
    const auto token = reboot::client_token();
    if (timeout_seconds <= 0 && !token.empty())
        timeout_seconds = 180;
    const int fd = open_client_socket(error, timeout_seconds);
    if (fd < 0) {
        return 1;
    }
    std::vector<std::string> request(args.begin() + static_cast<std::ptrdiff_t>(start), args.end());
    if (!token.empty())
        request.insert(request.begin(), {"@soft-reboot", token});
    if (!write_all(fd, join_request(request, 0))) {
        error = std::strerror(errno);
        close(fd);
        return 2;
    }
    shutdown(fd, SHUT_WR);
    const bool complete = read_all(fd, 1024UL * 1024, response);
    if (!complete)
        error = std::strerror(errno);
    close(fd);
    return complete ? 0 : 2;
}

bool daemon_running() {
    const std::vector<std::string> ping = {"daemon", "ping"};
    std::string response;
    std::string error;
    return send_request(ping, 0, response, error, 5) == 0 &&
           response.find("\"ok\":true") != std::string::npos;
}

int serve_foreground(int ready_fd = -1) {
    reboot::release();
    umask(0077);
    std::string preparation_error;
    if (!prepare_runtime(preparation_error) || !logging::prepare_boot_log(preparation_error)) {
        logging::write(logging::Level::Error, "daemon", preparation_error);
        std::cerr << preparation_error << '\n';
        return 1;
    }
    Config log_config;
    std::string config_error;
    if (read_config_file(log_config, config_error))
        logging::set_debug_enabled(log_config.debug || log_config.verbose);
    CommandCapture capture;
    if (!capture.prepare()) {
        std::cerr << "cannot allocate daemon output buffers\n";
        return 1;
    }
    std::string request;
    request.reserve(64UL * 1024);
    std::error_code ec;

    const auto socket_path = runtime_socket_file().string();
    if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "socket path is too long: " << socket_path << "\n";
        return 1;
    }

    const auto lock_path = runtime_daemon_lock_file();
    const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        std::cerr << "open " << lock_path << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "kagamid is already running\n";
        close(lock_fd);
        return 1;
    }

    // The lifetime lock proves there is no live owner before replacing a stale
    // socket. It also serializes concurrent first-use auto-start attempts.
    fs::remove(runtime_socket_file(), ec);
    const int server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        std::cerr << "socket: " << std::strerror(errno) << "\n";
        close(lock_fd);
        return 1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind " << socket_path << ": " << std::strerror(errno) << "\n";
        close(server_fd);
        close(lock_fd);
        return 1;
    }
    chmod(socket_path.c_str(), 0600);

    if (listen(server_fd, 8) != 0) {
        std::cerr << "listen: " << std::strerror(errno) << "\n";
        close(server_fd);
        close(lock_fd);
        return 1;
    }

    {
        std::ofstream pid(runtime_pid_file(), std::ios::trunc);
        if (pid) {
            pid << getpid() << "\n";
        }
    }
    append_log("ready pid=" + std::to_string(getpid()) + " socket=" + socket_path);
    if (ready_fd >= 0) {
        const char ready = 1;
        (void)send(ready_fd, &ready, 1, MSG_NOSIGNAL);
        close(ready_fd);
    }

    int mounts_fd = open("/proc/1/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mounts_fd < 0)
        append_log("cannot watch storage mounts: " + std::string(std::strerror(errno)),
                   logging::Level::Error);
    if (config_error.empty() && log_config.kasumi_enabled) {
        std::string restore_error;
        if (!mount::kasumi::restore_persisted_hide_rules(restore_error))
            append_log(restore_error, logging::Level::Error);
    }
    bool stopping = false;
    int exit_code = 0;
    while (!stopping) {
        if (reboot::recover_abandoned(ksud::recover_soft_reboot))
            append_log("recovered abandoned soft reboot transaction", logging::Level::Error);
        const bool pending_hides = mount::kasumi::has_pending_hide_rules();
        pollfd descriptors[] = {{server_fd, POLLIN, 0},
                                {pending_hides ? mounts_fd : -1, POLLPRI, 0}};
        const int ready = poll(descriptors, 2, 2000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            append_log(std::string("poll failed: ") + std::strerror(errno), logging::Level::Error);
            exit_code = 1;
            break;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            exit_code = 1;
            break;
        }
        if ((descriptors[1].revents & (POLLHUP | POLLNVAL)) != 0 ||
            ((descriptors[1].revents & POLLERR) != 0 && (descriptors[1].revents & POLLPRI) == 0)) {
            close(mounts_fd);
            mounts_fd = -1;
            append_log("storage mount watch lost", logging::Level::Error);
        }
        if ((descriptors[1].revents & POLLPRI) != 0)
            mount::kasumi::retry_pending_hide_rules();
        if ((descriptors[0].revents & POLLIN) == 0)
            continue;
        const int client_fd = accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            append_log(std::string("accept failed: ") + std::strerror(errno),
                       logging::Level::Error);
            continue;
        }

        timeval timeout = {};
        timeout.tv_sec = 5;
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        (void)read_all(client_fd, 64UL * 1024, request);
        auto request_args = split_request(request);
        reboot::set_request_token({});
        if (request_args.size() > 2 && request_args[0] == "@soft-reboot") {
            reboot::set_request_token(request_args[1]);
            request_args.erase(request_args.begin(), request_args.begin() + 2);
        }
        if (request_args.empty()) {
            append_log("rejected malformed or empty request", logging::Level::Warning);
            send_response(client_fd, false, 1, EINVAL, "", "empty daemon request");
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "stop") {
            if (reboot::active()) {
                send_response(client_fd, false, 1, EBUSY, "", "soft reboot is active\n");
                close(client_fd);
                continue;
            }
            send_response(client_fd, true, 0, 0, "stopping\n", "");
            stopping = true;
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 && request_args[1] == "ping") {
            send_response(client_fd, true, 0, 0, "pong\n", "");
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon" && request_args.size() >= 2 &&
            request_args[1] == "status") {
            send_response(client_fd, true, 0, 0, status_json(true), "");
            close(client_fd);
            continue;
        }

        if (request_args[0] == "daemon") {
            send_response(client_fd, false, 1, EINVAL, "",
                          "daemon control commands cannot be forwarded\n");
            close(client_fd);
            continue;
        }

        const auto result = run_command_capture(request_args, capture);
        reboot::set_request_token({});
        send_response(client_fd, result.exit_code == 0, result.exit_code, result.error_number,
                      result.stdout_text, result.stderr_text);
        close(client_fd);
    }

    append_log("kagamid stopped");
    if (mounts_fd >= 0)
        close(mounts_fd);
    close(server_fd);
    fs::remove(runtime_socket_file(), ec);
    fs::remove(runtime_pid_file(), ec);
    close(lock_fd);
    return exit_code;
}

int start_background(bool report_status) {
    if (daemon_running()) {
        return report_status ? print_status_json() : 0;
    }

    std::string preparation_error;
    if (!prepare_runtime(preparation_error) || !logging::prepare_boot_log(preparation_error)) {
        std::cerr << preparation_error << '\n';
        return 1;
    }
    int ready_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ready_pipe) != 0)
        return 1;
    const auto detached = ksud::daemonize_process(false);
    if (detached == ksud::ProcessDaemonizeResult::Error) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        std::cerr << "fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (detached == ksud::ProcessDaemonizeResult::Daemon) {
        close(ready_pipe[0]);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        const int log_fd = open(runtime_log_file().c_str(),
                                O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        _exit(serve_foreground(ready_pipe[1]));
    }
    close(ready_pipe[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0)
            break;
        pollfd descriptor{ready_pipe[0], POLLIN | POLLHUP, 0};
        const int ret = poll(&descriptor, 1, static_cast<int>(left));
        if (ret < 0 && errno == EINTR)
            continue;
        char ready = 0;
        const bool ok = ret > 0 && read(ready_pipe[0], &ready, 1) == 1 && ready == 1;
        close(ready_pipe[0]);
        if (!ok)
            return 1;
        return report_status ? print_status_json() : 0;
    }
    close(ready_pipe[0]);
    std::cerr << "kagamid did not become ready\n";
    return 1;
}
}  // namespace

int run_via_daemon(const std::vector<std::string>& args, bool display_errors) {
    request_completed = false;
    if (args.empty()) {
        return 1;
    }

    std::string response;
    std::string error;
    int request_status = send_request(args, 0, response, error);
    if (request_status == 1) {
        if (start_background(false) == 0)
            request_status = send_request(args, 0, response, error);
    }
    if (request_status != 0) {
        if (display_errors)
            return ksud::terminal::error(
                "mount daemon unavailable: " + error,
                "Inspect 'ksud kagami daemon status' and retry with --verbose.");
        std::cerr << "daemon unavailable: " << error << "\n";
        return 1;
    }

    JsonValue envelope;
    if (!parse_json(response, envelope, error) || !envelope.is_object()) {
        if (display_errors)
            return ksud::terminal::error("invalid mount daemon response: " + error);
        std::cerr << "invalid daemon response: " << error << "\n";
        return 1;
    }
    const JsonValue* code = envelope.find("exit_code");
    const JsonValue* out = envelope.find("stdout");
    const JsonValue* err = envelope.find("stderr");
    if (!code || !code->is_number() || !out || !out->is_string() || !err || !err->is_string()) {
        if (display_errors)
            return ksud::terminal::error("invalid mount daemon response fields");
        std::cerr << "invalid daemon response fields\n";
        return 1;
    }
    request_completed = true;
    std::cout << out->s;
    if (display_errors && code->n != 0 && !err->s.empty())
        ksud::terminal::diagnostics(err->s);
    else
        std::cerr << err->s;
    return static_cast<int>(code->n);
}

bool last_daemon_request_completed() {
    return request_completed;
}

bool daemon_supports_soft_reboot(std::string& error) {
    std::string response;
    const int rc = send_request({"daemon", "status"}, 0, response, error, 5);
    if (rc == 1 && (errno == ENOENT || errno == ECONNREFUSED)) {
        error.clear();
        return true;
    }
    JsonValue envelope;
    JsonValue status;
    if (rc != 0 || !parse_json(response, envelope, error) || !envelope.at("ok").bool_or(false) ||
        !parse_json(envelope.at("stdout").string_or(""), status, error) ||
        status.at("soft_reboot_protocol").u32_or(0) != 1) {
        error = "update/restart the running ksud mount controller for soft reboot support";
        return false;
    }
    error.clear();
    return true;
}

namespace {
int print_status_json() {
    const bool running = daemon_running();
    std::cout << status_json(running);
    return 0;
}
}  // namespace

int run_daemon_command(const std::vector<std::string>& args) {
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub == "status") {
        return print_status_json();
    }

    if (sub == "serve") {
        return serve_foreground();
    }
    if (sub == "start") {
        return start_background(true);
    }
    if (sub == "call") {
        if (args.size() < 3) {
            std::cerr << "daemon call requires a command\n";
            return 1;
        }
        std::string response;
        std::string error;
        const int rc = send_request(args, 2, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }
    if (sub == "ping" || sub == "stop") {
        std::string response;
        std::string error;
        const int rc = send_request(args, 0, response, error);
        if (rc != 0) {
            std::cerr << "daemon unavailable: " << error << "\n";
            return rc;
        }
        std::cout << response;
        return 0;
    }

    std::cerr << "usage: ksud kagami daemon status|start|serve|call|ping|stop\n";
    return 1;
}

}  // namespace kagami
