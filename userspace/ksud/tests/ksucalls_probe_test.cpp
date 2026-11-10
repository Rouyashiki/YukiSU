#include "../src/core/ksucalls.hpp"
#include "../src/defs.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>

namespace {
bool ready = false;
bool legacy = false;
int issued = -1;
int inherited = -1;
unsigned requests = 0;
}  // namespace

namespace ksud {
void log_v(const char*, ...) {}
void log_d(const char*, ...) {}
void log_i(const char*, ...) {}
void log_w(const char*, ...) {}
void log_e(const char*, ...) {}
}  // namespace ksud

extern "C" int __wrap_prctl(int option, ...) {
    if (option != KSU_PRCTL_GET_FD || !ready) {
        errno = EINVAL;
        return -1;
    }
    va_list args;
    va_start(args, option);
    auto* command = va_arg(args, int32_t*);
    va_end(args);
    issued = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(issued >= 0);
    ++requests;
    command[0] = 0;
    command[1] = issued;
    return 0;
}

extern "C" long __wrap_syscall(long number, ...) {
    assert(number == SYS_reboot);
    errno = EPERM;
    return -1;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert(fcntl(fd, F_GETFD) >= 0);
    const auto command = static_cast<uint32_t>(request);
    if (legacy && command == KSU_IOCTL_GET_INFO) {
        errno = ENOTTY;
        return -1;
    }
    assert(command == KSU_IOCTL_GET_INFO || command == KSU_IOCTL_GET_INFO_LEGACY);
    va_list args;
    va_start(args, request);
    auto* version = va_arg(args, uint32_t*);
    va_end(args);
    *version = 10709;
    return 0;
}

extern "C" ssize_t __real_readlink(const char*, char*, size_t);
extern "C" ssize_t __wrap_readlink(const char* path, char* data, size_t size) {
    if (inherited >= 0 && std::string(path) == "/proc/self/fd/" + std::to_string(inherited)) {
        constexpr char name[] = "anon_inode:[ksu_driver]";
        assert(size >= sizeof(name) - 1);
        std::memcpy(data, name, sizeof(name) - 1);
        return sizeof(name) - 1;
    }
    return __real_readlink(path, data, size);
}

int main() {
    assert(!ksud::has_kernelsu());
    ready = true;
    assert(ksud::has_kernelsu());
    assert(fcntl(issued, F_GETFD) == -1 && errno == EBADF);
    legacy = true;
    assert(ksud::has_kernelsu());
    assert(fcntl(issued, F_GETFD) == -1 && errno == EBADF);
    legacy = false;
    assert(ksud::get_version() == 10709);
    assert(fcntl(issued, F_GETFD) >= 0);
    inherited = issued;
    const unsigned before = requests;
    assert(ksud::has_kernelsu());
    assert(requests == before);
    assert(fcntl(inherited, F_GETFD) >= 0);
    close(inherited);
}
