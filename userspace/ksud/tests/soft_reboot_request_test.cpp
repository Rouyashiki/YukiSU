#include "../kagami/src/core/request_completion.hpp"

#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>

namespace {
void check_mount_worker_lifetime(bool controller_exited) {
    int request[2];
    int gate[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, request) == 0);
    assert(pipe2(gate, O_CLOEXEC) == 0);
    const pid_t worker = fork();
    assert(worker >= 0);
    if (worker == 0) {
        close(request[0]);
        close(gate[1]);
        char ready;
        assert(read(gate[0], &ready, 1) == 1);
        const std::array<char, 4096> tail{};
        for (int chunk = 0; chunk < 512; ++chunk) {
            size_t offset = 0;
            while (offset < tail.size()) {
                const auto sent =
                    send(request[1], tail.data() + offset, tail.size() - offset, MSG_NOSIGNAL);
                assert(sent > 0);
                offset += static_cast<size_t>(sent);
            }
        }
        close(request[1]);
        close(gate[0]);
        _exit(0);
    }
    close(gate[0]);
    if (controller_exited)
        close(request[1]);
    timeval timeout{};
    timeout.tv_usec = 20000;
    assert(setsockopt(request[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    char reply;
    assert(recv(request[0], &reply, 1, 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));

    std::promise<void> entered;
    auto started = entered.get_future();
    auto completion = std::async(std::launch::async, [&] {
        entered.set_value();
        return kagami::daemon_protocol::wait_for_request_completion(request[0]);
    });
    started.wait();
    assert(completion.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(write(gate[1], "x", 1) == 1);
    close(gate[1]);
    int status;
    assert(waitpid(worker, &status, 0) == worker);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (!controller_exited) {
        assert(completion.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        close(request[1]);
    }
    assert(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    assert(completion.get());
    close(request[0]);
}
}  // namespace

int main() {
    check_mount_worker_lifetime(false);
    check_mount_worker_lifetime(true);
    assert(!kagami::daemon_protocol::wait_for_request_completion(-1) && errno == EBADF);
    int request[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, request) == 0);
    assert(send(request[0], "unread", 6, MSG_NOSIGNAL) == 6);
    close(request[1]);
    assert(kagami::daemon_protocol::wait_for_request_completion(request[0]));
    close(request[0]);
    std::cout << "soft reboot requests: timeouts, retained worker sockets and resets passed\n";
}
