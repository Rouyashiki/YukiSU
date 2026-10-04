#include "zygiskd.hpp"

#include "core/restorecon.hpp"
#include "defs.hpp"

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace ksud {
namespace {

struct PrctlGetFdCmd {
  int32_t result;
  int32_t fd;
};

int g_driver_fd = -1;

int scan_driver_fd() {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == nullptr)
    return -1;

  int found = -1;
  while (dirent *entry = readdir(dir)) {
    char *end = nullptr;
    long number = strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0' || number < 0)
      continue;

    char path[64];
    char target[256];
    int len = snprintf(path, sizeof(path), "/proc/self/fd/%ld", number);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(path))
      continue;
    ssize_t got = readlink(path, target, sizeof(target) - 1);
    if (got <= 0)
      continue;
    target[got] = '\0';
    if (strstr(target, "[ksu_driver]") != nullptr) {
      found = static_cast<int>(number);
      break;
    }
  }
  closedir(dir);
  return found;
}

int get_driver_fd() {
  if (g_driver_fd >= 0)
    return g_driver_fd;

  g_driver_fd = scan_driver_fd();
  if (g_driver_fd >= 0)
    return g_driver_fd;

  PrctlGetFdCmd command{-1, -1};
  prctl(KSU_PRCTL_GET_FD, &command, 0, 0, 0);
  if (command.result == 0 && command.fd >= 0) {
    g_driver_fd = command.fd;
    return g_driver_fd;
  }

  int fd = -1;
  syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
  if (fd >= 0)
    g_driver_fd = fd;
  return g_driver_fd;
}

} // namespace

int ksuctl(int request, void *arg) {
  int fd = get_driver_fd();
  return fd < 0 || ioctl(fd, request, arg) < 0 ? -1 : 0;
}

bool uid_granted_root(uint32_t uid) {
  ksu_uid_granted_root_cmd command{};
  command.uid = uid;
  return ksuctl(KSU_IOCTL_UID_GRANTED_ROOT, &command) == 0 &&
         command.granted != 0;
}

bool uid_should_umount(uint32_t uid) {
  ksu_uid_should_umount_cmd command{};
  command.uid = uid;
  return ksuctl(KSU_IOCTL_UID_SHOULD_UMOUNT, &command) == 0 &&
         command.should_umount != 0;
}

bool lsetfilecon(const std::filesystem::path &path,
                 const std::string &context) {
  std::string current(context.size() + 1, '\0');
  const ssize_t length = lgetxattr(path.c_str(), "security.selinux",
                                   current.data(), current.size());
  if (length >= 0 &&
      (static_cast<size_t>(length) == context.size() ||
       (static_cast<size_t>(length) == context.size() + 1 &&
        current[context.size()] == '\0')) &&
      memcmp(current.data(), context.data(), context.size()) == 0)
    return true;
  return lsetxattr(path.c_str(), "security.selinux", context.c_str(),
                   context.size() + 1, 0) == 0;
}

} // namespace ksud

int main() { return zygiskd_main(); }
