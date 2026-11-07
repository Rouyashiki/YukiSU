#include "process_maps.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/sysmacros.h>

namespace {
std::string input;
bool open_failure = false;
bool read_failure = false;
bool allocation_failure = false;
size_t read_offset = 0;
ssize_t fail_read(void *, char *buffer, size_t capacity) {
  if (read_offset < input.size()) {
    const size_t count = std::min(capacity, input.size() - read_offset);
    std::copy_n(input.data() + read_offset, count, buffer);
    read_offset += count;
    return static_cast<ssize_t>(count);
  }
  errno = EIO;
  return -1;
}
} // namespace

void *operator new(size_t size) {
  if (allocation_failure)
    throw std::bad_alloc();
  if (void *value = malloc(size ? size : 1))
    return value;
  throw std::bad_alloc();
}
void operator delete(void *value) noexcept { free(value); }
void operator delete(void *value, size_t) noexcept { free(value); }

extern "C" FILE *__wrap_fopen(const char *path, const char *) {
  assert(strcmp(path, "/proc/self/maps") == 0);
  if (open_failure) {
    errno = EACCES;
    return nullptr;
  }
  if (read_failure) {
    cookie_io_functions_t functions{};
    functions.read = fail_read;
    return fopencookie(nullptr, "r", functions);
  }
  return fmemopen(input.data(), input.size(), "r");
}

extern "C" FILE *__wrap_fopen64(const char *path, const char *mode) {
  return __wrap_fopen(path, mode);
}

int main() {
  for (const std::string &path :
       {std::string(), std::string("/a path (deleted)"),
        std::string("[anon:test]"), std::string(17000, 'x')}) {
    for (bool newline : {false, true}) {
      input =
          "1000-2000 r--p 00000012 08:01 42    " + path + (newline ? "\n" : "");
      int error = -1;
      auto maps = yukizygisk::MapInfo::Scan(&error);
      assert(error == 0 && maps.size() == 1);
      const auto &map = maps[0];
      assert(map.start == 0x1000 && map.end == 0x2000 &&
             map.perms == PROT_READ && map.is_private && map.offset == 0x12 &&
             map.dev == makedev(8, 1) && map.inode == 42 && map.path == path);
    }
  }
  const char *invalid[] = {"1000-1000 r--p 0 00:00 0",
                           "1000-2000 r--q 0 00:00 0",
                           "1000-2000 r--p 0 00:00",
                           "1000-2000 r--p 10000000000000000 00:00 0",
                           "1000-2000 r--p 0 100000000:00 0",
                           "1000-2000 r--p 0 00:00 18446744073709551616",
                           "+1000-2000 r--p 0 00:00 0",
                           "0x1000-2000 r--p 0 00:00 0",
                           "1000-2000r--p 0 00:00 0",
                           "1000-2000 r--p 0 00:00 0/path"};
  for (const char *line : invalid) {
    input = std::string("100-200 rw-p 0 00:00 0\n") + line;
    int error = 0;
    assert(yukizygisk::MapInfo::Scan(&error).empty() && error == EINVAL);
  }
  input = "1000-3000 rw-p 0 00:00 0\n2000-4000 rw-p 0 00:00 0";
  int error = 0;
  assert(yukizygisk::MapInfo::Scan(&error).empty() && error == EINVAL);
  input = "1000-2000 rw-p 0 00:00 0 /path";
  input.append(1, '\0');
  input += "tail";
  assert(yukizygisk::MapInfo::Scan(&error).empty() && error == EINVAL);
  input = "1000-2000 rw-p 0 00:00 0 /path";
  allocation_failure = true;
  auto failed = yukizygisk::MapInfo::Scan(&error);
  allocation_failure = false;
  assert(failed.empty() && error == ENOMEM);
  open_failure = true;
  assert(yukizygisk::MapInfo::Scan(&error).empty() && error == EACCES);
  open_failure = false;
  input += '\n';
  read_offset = 0;
  read_failure = true;
  assert(yukizygisk::MapInfo::Scan(&error).empty() && error == EIO);
  read_failure = false;
  assert(yukizygisk::MapInfo::Scan(&error).size() == 1 && error == 0);
  puts("process maps parsing and failure handling passed");
}
