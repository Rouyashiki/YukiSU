#include "process_maps.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sys/mman.h>
#include <sys/sysmacros.h>

namespace yukizygisk {
namespace {
bool space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

bool separator(const char *&cursor, const char *end) noexcept {
  if (cursor == end || !space(*cursor))
    return false;
  do {
    ++cursor;
  } while (cursor != end && space(*cursor));
  return true;
}

bool consume(const char *&cursor, const char *end, char expected) noexcept {
  if (cursor == end || *cursor != expected)
    return false;
  ++cursor;
  return true;
}

template <typename T>
bool number(const char *&cursor, const char *end, T &value, int base) noexcept {
  auto result = std::from_chars(cursor, end, value, base);
  if (result.ec != std::errc{})
    return false;
  cursor = result.ptr;
  return true;
}

bool parse(const char *line, size_t length, MapInfo &map) {
  const char *cursor = line;
  const char *end = line + length;
  uintptr_t begin = 0, limit = 0;
  uint64_t offset = 0, inode = 0;
  unsigned major_id = 0, minor_id = 0;
  if (!number(cursor, end, begin, 16) || !consume(cursor, end, '-') ||
      !number(cursor, end, limit, 16) || begin >= limit ||
      !separator(cursor, end) || end - cursor < 4)
    return false;
  const char r = cursor[0], w = cursor[1], x = cursor[2], p = cursor[3];
  if ((r != 'r' && r != '-') || (w != 'w' && w != '-') ||
      (x != 'x' && x != '-') || (p != 'p' && p != 's'))
    return false;
  cursor += 4;
  if (!separator(cursor, end) || !number(cursor, end, offset, 16) ||
      !separator(cursor, end) || !number(cursor, end, major_id, 16) ||
      !consume(cursor, end, ':') || !number(cursor, end, minor_id, 16) ||
      !separator(cursor, end) || !number(cursor, end, inode, 10) ||
      inode > std::numeric_limits<ino_t>::max())
    return false;
  if (cursor != end && !separator(cursor, end))
    return false;
  if (end != cursor && end[-1] == '\n')
    --end;
  for (const char *it = cursor; it != end; ++it)
    if (*it == '\0')
      return false;
  map.start = begin;
  map.end = limit;
  map.perms = (r == 'r' ? PROT_READ : 0) | (w == 'w' ? PROT_WRITE : 0) |
              (x == 'x' ? PROT_EXEC : 0);
  map.is_private = p == 'p';
  map.offset = offset;
  map.dev = makedev(major_id, minor_id);
  map.inode = static_cast<ino_t>(inode);
  map.path.assign(cursor, end);
  return true;
}
} // namespace

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
    ssize_t length = 0;
    while ((length = getline(&line, &capacity, stream.get())) >= 0) {
      MapInfo map;
      if (!parse(line, static_cast<size_t>(length), map) ||
          (!maps.empty() && map.start < maps.back().end)) {
        if (error != nullptr)
          *error = EINVAL;
        valid = false;
        break;
      }
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
