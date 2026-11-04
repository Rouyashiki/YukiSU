// Exercise the private production runner with linker-injected syscall failures.
#include "../../src/plugin/lua_engine.cpp"  // NOLINT(bugprone-suspicious-include)

#include <cassert>

namespace {
enum class Fault : uint8_t {
    None,
    Pipe,
    Fork,
    Input,
    Dup,
    Exec,
    StatusRead,
    OutputRead,
    Poll,
    Wait,
    Nonblock
};
Fault fault = Fault::None;
int pipe_calls = 0;

bool fail_once(Fault operation) {
    if (fault != operation)
        return false;
    fault = Fault::None;
    errno = EIO;
    return true;
}

size_t descriptors() {
    return static_cast<size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                                             std::filesystem::directory_iterator()));
}
}  // namespace

// GNU ld --wrap requires these symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" {
int __real_pipe2(int*, int);
pid_t __real_fork();
int __real_open(const char*, int, ...);
int __real_dup2(int, int);
int __real_execvp(const char*, char* const*);
ssize_t __real_read(int, void*, size_t);
int __real_poll(pollfd*, nfds_t, int);
pid_t __real_waitpid(pid_t, int*, int);
int __real_fcntl(int, int, ...);

int __wrap_pipe2(int* fds, int flags) {
    if (++pipe_calls == 2 && fail_once(Fault::Pipe))
        return -1;
    return __real_pipe2(fds, flags);
}

pid_t __wrap_fork() {
    return fail_once(Fault::Fork) ? -1 : __real_fork();
}

int __wrap_open(const char* path, int flags, ...) {
    if (strcmp(path, "/dev/null") == 0 && fail_once(Fault::Input))
        return -1;
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, mode_t);
        va_end(arguments);
    }
    return __real_open(path, flags, mode);
}

int __wrap_dup2(int source, int target) {
    return fail_once(Fault::Dup) ? -1 : __real_dup2(source, target);
}

int __wrap_execvp(const char* program, char* const* arguments) {
    return fail_once(Fault::Exec) ? -1 : __real_execvp(program, arguments);
}

ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    if (fail_once(Fault::StatusRead) ||
        ((__real_fcntl(fd, F_GETFL) & O_NONBLOCK) != 0 && fail_once(Fault::OutputRead)))
        return -1;
    return __real_read(fd, buffer, size);
}

int __wrap_poll(pollfd* events, nfds_t count, int timeout) {
    return fail_once(Fault::Poll) ? -1 : __real_poll(events, count, timeout);
}

pid_t __wrap_waitpid(pid_t pid, int* status, int flags) {
    return fail_once(Fault::Wait) ? -1 : __real_waitpid(pid, status, flags);
}

int __wrap_fcntl(int fd, int operation, ...) {
    if (operation == F_GETFD || operation == F_GETFL)
        return __real_fcntl(fd, operation);
    if (operation == F_SETFL && fail_once(Fault::Nonblock))
        return -1;
    va_list arguments;
    va_start(arguments, operation);
    const int value = va_arg(arguments, int);
    va_end(arguments);
    return __real_fcntl(fd, operation, value);
}
}  // extern "C"
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

int main() {
    const size_t initial = descriptors();
    for (const auto operation :
         {Fault::Pipe, Fault::Fork, Fault::Input, Fault::Dup, Fault::Exec, Fault::StatusRead,
          Fault::OutputRead, Fault::Poll, Fault::Wait, Fault::Nonblock}) {
        fault = operation;
        pipe_calls = 0;
        const auto result = ksud::run_command(
            {"sh", "-c", "printf output; printf error >&2; exec >/dev/null 2>&1; exec sleep 30"});
        fault = Fault::None;
        assert(!result.error.empty());
        assert(descriptors() == initial);
        int status = 0;
        assert(__real_waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    }
    fault = Fault::Fork;
    assert(!ksud::set_prop_builtin("test.key", "value"));
    fault = Fault::None;
    assert(descriptors() == initial);
    puts("Command syscall failures close descriptors and reap owned children");
}
