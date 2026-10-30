#include "process_maps.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sys/mman.h>
#include <sys/sysmacros.h>

namespace yukizygisk {

std::vector<MapInfo> MapInfo::Scan(int *error) noexcept {
  if (error != nullptr)
    *error = 0;
  std::vector<MapInfo> maps;
  std::unique_ptr<FILE, decltype(&fclose)> stream(
      fopen("/proc/self/maps", "re"), fclose);
  if (!stream) {
    if (error != nullptr)
      *error = errno;
    return maps;
  }
  char *line = nullptr;
  size_t capacity = 0;
  bool valid = true;
  try {
    while (getline(&line, &capacity, stream.get()) >= 0) {
      unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
      unsigned major_id = 0, minor_id = 0;
      char permissions[5]{};
      int consumed = 0;
      if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &begin, &end,
                 permissions, &offset, &major_id, &minor_id, &inode,
                 &consumed) != 7 ||
          consumed == 0 || begin >= end || end > UINTPTR_MAX ||
          inode > std::numeric_limits<ino_t>::max() ||
          (!maps.empty() && begin < maps.back().end) ||
          (permissions[0] != 'r' && permissions[0] != '-') ||
          (permissions[1] != 'w' && permissions[1] != '-') ||
          (permissions[2] != 'x' && permissions[2] != '-') ||
          (permissions[3] != 'p' && permissions[3] != 's')) {
        if (error != nullptr)
          *error = EINVAL;
        valid = false;
        break;
      }
      MapInfo map;
      map.start = static_cast<uintptr_t>(begin);
      map.end = static_cast<uintptr_t>(end);
      map.perms = (permissions[0] == 'r' ? PROT_READ : 0) |
                  (permissions[1] == 'w' ? PROT_WRITE : 0) |
                  (permissions[2] == 'x' ? PROT_EXEC : 0);
      map.is_private = permissions[3] == 'p';
      map.offset = offset;
      map.dev = makedev(major_id, minor_id);
      map.inode = static_cast<ino_t>(inode);
      map.path = line + consumed;
      if (!map.path.empty() && map.path.back() == '\n')
        map.path.pop_back();
      maps.push_back(std::move(map));
    }
  } catch (const std::bad_alloc &) {
    if (error != nullptr)
      *error = ENOMEM;
    valid = false;
  } catch (...) {
    if (error != nullptr)
      *error = EINVAL;
    valid = false;
  }
  if (ferror(stream.get()) != 0 || !feof(stream.get())) {
    if (error != nullptr && *error == 0)
      *error = errno != 0 ? errno : EIO;
    valid = false;
  }
  free(line);
  if (!valid)
    maps.clear();
  return maps;
}

} // namespace yukizygisk
