#pragma once

#include "daemon_health.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace yukizygisk::health {

inline int open_recovery_directory(const char *path = kRecoveryDirectory) {
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return -1;
  const int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  struct stat status{};
  if (fstat(fd, &status) != 0 || status.st_uid != 0 ||
      (status.st_mode & 0777) != 0700) {
    close(fd);
    errno = EACCES;
    return -1;
  }
  return fd;
}

inline int lock_daemon(uint32_t abi, const char *path = kRecoveryDirectory) {
  const int directory = open_recovery_directory(path);
  if (directory < 0)
    return -1;
  const int fd =
      openat(directory, abi == 1 ? "daemon32.lock" : "daemon64.lock",
             O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
  const int error = errno;
  close(directory);
  if (fd < 0) {
    errno = error;
    return -1;
  }
  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != 0 || status.st_nlink != 1 ||
      (status.st_mode & 0777) != 0600) {
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

inline uint64_t process_start_ticks(pid_t pid) {
  char path[64];
  (void)snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return 0;
  char buffer[4096];
  ssize_t length;
  do {
    length = read(fd, buffer, sizeof(buffer) - 1);
  } while (length < 0 && errno == EINTR);
  close(fd);
  if (length <= 0)
    return 0;
  buffer[length] = '\0';
  const char *end = strrchr(buffer, ')');
  if (end == nullptr)
    return 0;
  std::string_view remaining(end + 1);
  for (unsigned field = 3; field <= 22; ++field) {
    const auto begin = remaining.find_first_not_of(' ');
    if (begin == std::string_view::npos)
      return 0;
    remaining.remove_prefix(begin);
    const auto separator = remaining.find(' ');
    const auto value = remaining.substr(0, separator);
    if (field == 22) {
      uint64_t result = 0;
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), result);
      return parsed.ec == std::errc{} &&
                     parsed.ptr == value.data() + value.size()
                 ? result
                 : 0;
    }
    if (separator == std::string_view::npos)
      return 0;
    remaining.remove_prefix(separator + 1);
  }
  return 0;
}

} // namespace yukizygisk::health
