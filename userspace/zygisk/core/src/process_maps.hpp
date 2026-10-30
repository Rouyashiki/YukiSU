#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace yukizygisk {

struct MapInfo {
  uintptr_t start = 0;
  uintptr_t end = 0;
  int perms = 0;
  bool is_private = false;
  uint64_t offset = 0;
  dev_t dev = 0;
  ino_t inode = 0;
  std::string path;

  static std::vector<MapInfo> Scan(int *error = nullptr) noexcept;
};

} // namespace yukizygisk
