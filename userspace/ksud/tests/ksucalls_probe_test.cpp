#include "../src/core/feature.hpp"
#include "../src/core/ksucalls.hpp"
#include "../src/defs.hpp"
#include "../src/module/module.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
bool ready = false;
bool reboot_ready = false;
bool legacy = false;
int issued = -1;
int inherited = -1;
unsigned requests = 0;
int magisk_flags = 0;
int classic_flags = 0;
bool module_managed = false;
}  // namespace

namespace ksud {
void log_v([[maybe_unused]] const char* format, ...) {}
void log_d([[maybe_unused]] const char* format, ...) {}
void log_i([[maybe_unused]] const char* format, ...) {}
void log_w([[maybe_unused]] const char* format, ...) {}
void log_e([[maybe_unused]] const char* format, ...) {}
std::map<std::string, std::vector<std::string>> get_managed_features() {
    if (module_managed)
        return {{"owner", {"magisk_compat"}}};
    return {};
}
}  // namespace ksud

// GNU ld --wrap requires these symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
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
    va_list args;
    va_start(args, number);
    assert(va_arg(args, unsigned int) == KSU_INSTALL_MAGIC1);
    assert(va_arg(args, unsigned int) == KSU_INSTALL_MAGIC2);
    assert(va_arg(args, int) == 0);
    auto* driver_fd = va_arg(args, int*);
    va_end(args);
    assert(*driver_fd == -1);
    if (reboot_ready) {
        *driver_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(*driver_fd >= 0);
    }
    errno = EPERM;
    return -1;
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    assert(fcntl(fd, F_GETFD) >= 0);
    const auto command = static_cast<uint32_t>(request);
    if (command == KSU_IOCTL_GET_FEATURE) {
        va_list args;
        va_start(args, request);
        auto* feature = va_arg(args, ksud::GetFeatureCmd*);
        va_end(args);
        assert(feature->feature_id == KSU_FEATURE_MAGISK_COMPAT ||
               feature->feature_id == KSU_FEATURE_SU_COMPAT);
        const int flags =
            feature->feature_id == KSU_FEATURE_MAGISK_COMPAT ? magisk_flags : classic_flags;
        if (flags < 0) {
            errno = EIO;
            return -1;
        }
        feature->supported = static_cast<uint8_t>(flags);
        return 0;
    }
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
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

int main(int argc, char** argv) {
    if (argc >= 3) {
        ready = true;
        magisk_flags = std::stoi(argv[1]);
        classic_flags = std::stoi(argv[2]);
        module_managed = argc == 4;
        std::printf("exit=%d\n", ksud::feature_check("magisk_compat"));
        return 0;
    }
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

    assert(ksud::open_driver_fd() == -1);
    assert(requests == before);
    reboot_ready = true;
    const int first = ksud::open_driver_fd();
    const int second = ksud::open_driver_fd();
    assert(first >= 0 && second >= 0 && first != second);
    assert(first != inherited && second != inherited);
    assert(requests == before);
    assert((fcntl(first, F_GETFD) & FD_CLOEXEC) != 0);
    close(first);
    assert(fcntl(second, F_GETFD) >= 0);
    assert(fcntl(inherited, F_GETFD) >= 0);
    close(second);
    close(inherited);
}
