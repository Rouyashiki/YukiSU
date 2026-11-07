// Exercise the private JNI client without adding a production test entry point.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../../manager/app/src/main/cpp/kagami_jni.cpp"

#include <cassert>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::string test_root;
pid_t server_child = -1;
pid_t listener_owner = -1;
int connect_signal = -1;
int starts = 0;

std::string test_socket() {
    return test_root + "/kagamid.sock";
}
std::string test_lock() {
    return test_root + "/kagamid.requests.lock";
}

void check_child(pid_t child) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void assert_request_pinned() {
    const Fd gate(open(test_lock().c_str(), O_RDWR | O_CLOEXEC));
    assert(gate.get() >= 0);
    assert(flock(gate.get(), LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK);
}

pid_t serve_one(bool crash, int inherited_lock = -1) {
    listener_owner = getpid();
    std::filesystem::remove(test_socket());
    const Fd listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    assert(listener.get() >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = test_socket();
    assert(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    assert(bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(listen(listener.get(), 1) == 0);
    const auto child = fork();
    assert(child >= 0);
    if (child == 0) {
        if (inherited_lock >= 0)
            close(inherited_lock);
        const Fd client(accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        assert(client.get() >= 0);
        assert_request_pinned();
        std::string request;
        char byte;
        while (read(client.get(), &byte, 1) == 1 && byte != '\n')
            request += byte;
        assert(!request.empty());
        std::ofstream(test_root + "/requests", std::ios::app) << request << '\n';
        assert_request_pinned();
        if (!crash) {
            constexpr char response[] = "{\"ok\":true}\n";
            assert(send(client.get(), response, sizeof(response) - 1, MSG_NOSIGNAL) ==
                   sizeof(response) - 1);
        }
        _exit(0);
    }
    return child;
}

void pinned_connection_test() {
    server_child = serve_one(false);
    const int gate = open(test_lock().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    assert(gate >= 0 && flock(gate, LOCK_EX) == 0);
    int signal[2];
    assert(pipe2(signal, O_CLOEXEC) == 0);
    const auto client = fork();
    assert(client >= 0);
    if (client == 0) {
        close(gate);
        close(signal[0]);
        connect_signal = signal[1];
        assert(request_controller("/test/ksud", "[\"count\",\"pinned\"]") == "{\"ok\":true}\n");
        _exit(0);
    }
    close(signal[1]);
    pollfd attempt{signal[0], POLLIN, 0};
    assert(poll(&attempt, 1, 200) == 0);
    close(gate);
    assert(poll(&attempt, 1, 2000) == 1 && (attempt.revents & POLLIN));
    close(signal[0]);
    check_child(client);
    check_child(server_child);
}
}  // namespace

extern "C" int real_open(const char* path, int flags, ...) asm("__real_open");
extern "C" int wrap_open(const char* path, int flags, ...) asm("__wrap_open");
extern "C" int wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    const auto redirected = std::strcmp(path, request_lock_path) == 0 ? test_lock() : path;
    return real_open(redirected.c_str(), flags, mode);
}

extern "C" int real_connect(int fd, const sockaddr* address,
                            socklen_t length) asm("__real_connect");
extern "C" int wrap_connect(int fd, const sockaddr* address,
                            socklen_t length) asm("__wrap_connect");
extern "C" int wrap_connect(int fd, const sockaddr* address, socklen_t length) {
    assert(address->sa_family == AF_UNIX && length == sizeof(sockaddr_un));
    auto redirected = *reinterpret_cast<const sockaddr_un*>(address);
    assert(std::strcmp(redirected.sun_path, socket_path) == 0);
    const auto path = test_socket();
    assert(path.size() < sizeof(redirected.sun_path));
    std::memset(redirected.sun_path, 0, sizeof(redirected.sun_path));
    std::memcpy(redirected.sun_path, path.c_str(), path.size() + 1);
    if (connect_signal >= 0)
        assert(write(connect_signal, "1", 1) == 1);
    return real_connect(fd, reinterpret_cast<const sockaddr*>(&redirected), length);
}

extern "C" int real_getsockopt(int fd, int level, int option, void* value,
                               socklen_t* size) asm("__real_getsockopt");
extern "C" int wrap_getsockopt(int fd, int level, int option, void* value,
                               socklen_t* size) asm("__wrap_getsockopt");
extern "C" int wrap_getsockopt(int fd, int level, int option, void* value, socklen_t* size) {
    const int result = real_getsockopt(fd, level, option, value, size);
    assert(result == 0 && level == SOL_SOCKET && option == SO_PEERCRED);
    auto* peer = static_cast<ucred*>(value);
    assert(peer->pid == listener_owner && peer->uid == getuid());
    peer->uid = 0;
    return result;
}

extern "C" int wrap_posix_spawn(pid_t* pid, const char* path,
                                const posix_spawn_file_actions_t* actions,
                                const posix_spawnattr_t* attributes, char* const args[],
                                char* const environment[]) asm("__wrap_posix_spawn");
extern "C" int wrap_posix_spawn(pid_t* pid, const char* path,
                                const posix_spawn_file_actions_t* /*actions*/,
                                const posix_spawnattr_t* /*attributes*/, char* const args[],
                                char* const /*environment*/[]) {
    assert(std::strcmp(path, "/test/ksud") == 0);
    assert(std::strcmp(args[1], "kagami") == 0 && std::strcmp(args[3], "start") == 0);
    const Fd gate(open(test_lock().c_str(), O_RDWR | O_CLOEXEC));
    assert(gate.get() >= 0 && flock(gate.get(), LOCK_EX | LOCK_NB) == 0);
    ++starts;
    server_child = serve_one(false, gate.get());
    *pid = fork();
    assert(*pid >= 0);
    if (*pid == 0)
        _exit(0);
    return 0;
}

int main() {
    char directory[] = "/tmp/yukisu-kagami-manager-XXXXXX";
    assert(mkdtemp(directory));
    test_root = directory;
    pinned_connection_test();
    assert(request_controller("/test/ksud", "[\"count\",\"started\"]") == "{\"ok\":true}\n");
    assert(starts == 1);
    check_child(server_child);
    {
        const Fd gate(open(test_lock().c_str(), O_RDWR | O_CLOEXEC));
        assert(gate.get() >= 0 && flock(gate.get(), LOCK_EX | LOCK_NB) == 0);
    }
    server_child = serve_one(true);
    bool failed = false;
    try {
        (void)request_controller("/test/ksud", "[\"count\",\"once\"]");
    } catch (const std::runtime_error&) {
        failed = true;
    }
    assert(failed && starts == 1);
    check_child(server_child);
    std::ifstream input(test_root + "/requests");
    std::string line;
    for (const auto* id : {"pinned", "started", "once"}) {
        assert(std::getline(input, line));
        assert(line == std::string("[\"count\",\"") + id + "\"]");
    }
    assert(!std::getline(input, line));
    std::filesystem::remove_all(test_root);
    std::cout << "Manager request locking, startup, and no-replay passed\n";
}
