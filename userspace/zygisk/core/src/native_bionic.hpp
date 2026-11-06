#pragma once

#include <android/dlext.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace yukizygisk::native_bionic {

struct Map {
  uintptr_t start = 0;
  uintptr_t end = 0;
  uint64_t offset = 0;
  uint64_t dev = 0;
  uint64_t inode = 0;
  int perms = 0;
};
using Maps = std::vector<Map>;
Maps read_maps();
using OpenExt = void *(*)(const char *, int, const android_dlextinfo *,
                          const void *);
using Iterate = int (*)(int (*)(dl_phdr_info *, size_t, void *), void *);
using AddressInfo = int (*)(const void *, Dl_info *);
using Lookup = void *(*)(void *, const char *, const void *);
using LookupVersion = void *(*)(void *, const char *, const char *,
                                const void *);

enum class Stage { Linker, Anchor, Revalidate, Load };
enum class Error {
  None,
  MapsUnavailable,
  InvalidElf,
  IdentityMismatch,
  SymbolUnavailable,
  SymbolAmbiguous,
  AnchorUnavailable,
  AnchorAmbiguous,
  LoaderFailure,
};

struct Failure {
  Stage stage = Stage::Linker;
  Error error = Error::None;
  int system_error = 0;
};

struct Identity {
  uint64_t device = 0;
  uint64_t inode = 0;
  bool matches(const Map &map) const {
    return inode != 0 && device == map.dev && inode == map.inode;
  }
};

struct Object {
  Identity identity;
  std::string path;
  uintptr_t bias = 0;
  uintptr_t base = 0;
};

struct LoaderApi {
  OpenExt open = nullptr;
  Iterate iterate = nullptr;
  AddressInfo address_info = nullptr;
  Lookup lookup = nullptr;
  LookupVersion lookup_version = nullptr;
  Object linker;
  std::array<uint64_t, 5> file_offsets{};
};

struct CallerContext {
  LoaderApi api;
  Object libc;
  uintptr_t anchor = 0;
  uintptr_t segment_end = 0;
  uint64_t file_offset = 0;
  uintptr_t reference = 0;
  uint64_t reference_offset = 0;
};

struct LoadResult {
  void *handle = nullptr;
  Failure failure;
  std::string loader_error;
  CallerContext context;
};

inline uintptr_t code_address(uintptr_t address) {
#if defined(__arm__)
  return address & ~static_cast<uintptr_t>(1);
#else
  return address;
#endif
}

inline size_t instruction_size(uintptr_t address) {
#if defined(__arm__)
  return (address & 1) != 0 ? 2 : 4;
#else
  (void)address;
  return 4;
#endif
}

inline bool add_address(uintptr_t base, uint64_t offset, uintptr_t &result) {
  if (offset > UINTPTR_MAX - base)
    return false;
  result = base + static_cast<uintptr_t>(offset);
  return true;
}

inline bool mapped_range(const Maps &maps, uintptr_t address, size_t size,
                         int permissions, const Identity *identity = nullptr,
                         const uint64_t *file_offset = nullptr) {
  uintptr_t end = 0;
  if (size == 0 || !add_address(address, size, end))
    return false;
  uintptr_t cursor = address;
  for (const auto &map : maps) {
    if (map.end <= cursor)
      continue;
    if (map.start > cursor || (map.perms & permissions) != permissions ||
        (identity != nullptr && !identity->matches(map)))
      return false;
    if (file_offset != nullptr) {
      const uint64_t relative = cursor - address;
      const uint64_t in_map = cursor - map.start;
      if (relative > UINT64_MAX - *file_offset ||
          in_map > UINT64_MAX - map.offset ||
          *file_offset + relative != map.offset + in_map)
        return false;
    }
    if (map.end >= end)
      return true;
    cursor = map.end;
  }
  return false;
}

bool resolve_loader(const Maps &maps, LoaderApi &api, Failure &failure);
bool find_caller(const LoaderApi &api, const Maps &maps,
                 const void *libc_symbol, CallerContext &context,
                 Failure &failure);
bool validate_context(const CallerContext &context, const Maps &maps,
                      Failure &failure);
bool prepare_caller(CallerContext &context, Failure &failure);
LoadResult load(const char *path, int flags, const android_dlextinfo &ext);
const char *stage_name(Stage stage);
const char *error_name(Error error);

} // namespace yukizygisk::native_bionic
