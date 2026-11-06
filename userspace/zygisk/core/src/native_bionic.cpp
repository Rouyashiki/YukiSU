#include "native_bionic.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace yukizygisk::native_bionic {
namespace {

bool fail(Failure &failure, Error error, int system_error = 0) {
  failure.error = error;
  failure.system_error = system_error;
  return false;
}

Identity identity_of(const struct stat &st) {
  return {static_cast<uint64_t>(st.st_dev), static_cast<uint64_t>(st.st_ino)};
}

bool same_identity(const Identity &left, const Identity &right) {
  return left.inode != 0 && left.device == right.device &&
         left.inode == right.inode;
}

bool native_machine(ElfW(Half) machine) {
#if defined(__aarch64__)
  return machine == EM_AARCH64;
#elif defined(__arm__)
  return machine == EM_ARM;
#else
  (void)machine;
  return false;
#endif
}

struct Scan {
  const Maps &maps;
  const Object &expected;
  uint64_t file_size = 0;
  uintptr_t page_size = 0;
  uintptr_t reference = 0;
  CallerContext &context;
  Failure &failure;
  size_t candidates = 0;
  bool valid = true;
};

int collect_libc(dl_phdr_info *info, size_t info_size, void *opaque) {
  auto &scan = *static_cast<Scan *>(opaque);
  if (info_size <
      offsetof(dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum)) {
    scan.valid = fail(scan.failure, Error::InvalidElf);
    return 1;
  }
  if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0')
    return 0;
  struct stat st{};
  if (stat(info->dlpi_name, &st) != 0)
    return 0;
  if (!same_identity(identity_of(st), scan.expected.identity))
    return 0;
  const uintptr_t phdr = reinterpret_cast<uintptr_t>(info->dlpi_phdr);
  if (info->dlpi_phnum == 0 || info->dlpi_phnum > 128 ||
      phdr % alignof(ElfW(Phdr)) != 0 ||
      !mapped_range(scan.maps, phdr, info->dlpi_phnum * sizeof(ElfW(Phdr)),
                    PROT_READ, &scan.expected.identity)) {
    scan.valid = fail(scan.failure, Error::InvalidElf);
    return 1;
  }
  CallerContext context;
  context.libc = scan.expected;
  context.libc.bias = info->dlpi_addr;
  uintptr_t lowest = UINTPTR_MAX;
  bool headers = false;
  bool contains_reference = false;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const auto &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD)
      continue;
    uintptr_t start = 0;
    uintptr_t end = 0;
    if (ph.p_filesz > ph.p_memsz || ph.p_offset > scan.file_size ||
        ph.p_filesz > scan.file_size - ph.p_offset ||
        !add_address(info->dlpi_addr, ph.p_vaddr, start) ||
        !add_address(start, ph.p_memsz, end) ||
        ph.p_offset % scan.page_size != ph.p_vaddr % scan.page_size) {
      scan.valid = fail(scan.failure, Error::InvalidElf);
      return 1;
    }
    if (ph.p_memsz == 0)
      continue;
    const uintptr_t page_start = start & ~(scan.page_size - 1);
    lowest = std::min(page_start, lowest);
    if ((ph.p_flags & PF_X) != 0 && scan.reference >= start &&
        scan.reference - start < ph.p_filesz)
      contains_reference = true;
    if ((ph.p_flags & PF_R) == 0 || ph.p_filesz == 0)
      continue;
    const uint64_t offset = ph.p_offset;
    if (!mapped_range(scan.maps, start, ph.p_filesz, PROT_READ,
                      &scan.expected.identity, &offset)) {
      scan.valid = fail(scan.failure, Error::IdentityMismatch);
      return 1;
    }
    if (ph.p_offset == 0 && ph.p_filesz >= sizeof(ElfW(Ehdr))) {
      if (start % alignof(ElfW(Ehdr)) != 0) {
        scan.valid = fail(scan.failure, Error::InvalidElf);
        return 1;
      }
      const auto &eh = *reinterpret_cast<const ElfW(Ehdr) *>(start);
      uintptr_t expected_phdr = 0;
      headers =
          memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
          eh.e_ident[EI_CLASS] ==
              (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) &&
          eh.e_ident[EI_DATA] == ELFDATA2LSB &&
          eh.e_ident[EI_VERSION] == EV_CURRENT && eh.e_version == EV_CURRENT &&
          eh.e_type == ET_DYN && native_machine(eh.e_machine) &&
          eh.e_ehsize == sizeof(ElfW(Ehdr)) &&
          eh.e_phentsize == sizeof(ElfW(Phdr)) &&
          eh.e_phnum == info->dlpi_phnum &&
          add_address(start, eh.e_phoff, expected_phdr) &&
          expected_phdr == phdr && eh.e_phoff <= ph.p_filesz &&
          info->dlpi_phnum * sizeof(ElfW(Phdr)) <= ph.p_filesz - eh.e_phoff;
      if (!headers) {
        scan.valid = fail(scan.failure, Error::InvalidElf);
        return 1;
      }
    }
    if (context.anchor == 0 || start < context.anchor) {
      context.anchor = start;
      context.segment_end = end;
      context.file_offset = offset;
    }
  }
  if (!contains_reference)
    return 0;
  if (!headers || lowest == UINTPTR_MAX || context.anchor == 0) {
    scan.valid = fail(scan.failure, Error::AnchorUnavailable);
    return 1;
  }
  if (lowest != scan.expected.base) {
    scan.valid = fail(scan.failure, Error::IdentityMismatch);
    return 1;
  }
  if (++scan.candidates != 1) {
    scan.valid = fail(scan.failure, Error::AnchorAmbiguous);
    return 1;
  }
  context.libc.base = lowest;
  scan.context.libc = std::move(context.libc);
  scan.context.anchor = context.anchor;
  scan.context.segment_end = context.segment_end;
  scan.context.file_offset = context.file_offset;
  return 0;
}

} // namespace

Maps read_maps() {
  Maps maps;
  FILE *file = fopen("/proc/self/maps", "re");
  if (file == nullptr)
    return maps;
  char *line = nullptr;
  size_t capacity = 0;
  bool valid = true;
  while (getline(&line, &capacity, file) != -1) {
    unsigned long long start = 0, end = 0, offset = 0, inode = 0;
    unsigned int major_device = 0, minor_device = 0;
    char perms[5]{};
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu", &start, &end, perms,
               &offset, &major_device, &minor_device, &inode) != 7 ||
        start >= end || end > UINTPTR_MAX ||
        (!maps.empty() && start < maps.back().end)) {
      valid = false;
      break;
    }
    maps.push_back(
        {static_cast<uintptr_t>(start), static_cast<uintptr_t>(end), offset,
         static_cast<uint64_t>(makedev(major_device, minor_device)), inode,
         (perms[0] == 'r' ? PROT_READ : 0) |
             (perms[1] == 'w' ? PROT_WRITE : 0) |
             (perms[2] == 'x' ? PROT_EXEC : 0)});
  }
  const bool read_error = ferror(file) != 0;
  free(line);
  const bool close_error = fclose(file) != 0;
  if (!valid || read_error || close_error)
    maps.clear();
  return maps;
}

bool find_caller(const LoaderApi &api, const Maps &maps,
                 const void *libc_symbol, CallerContext &context,
                 Failure &failure) {
  failure.stage = Stage::Anchor;
  context = {};
  context.api = api;
  if (api.iterate == nullptr || api.address_info == nullptr ||
      libc_symbol == nullptr)
    return fail(failure, Error::AnchorUnavailable);
  Dl_info provider{};
  if (api.address_info(libc_symbol, &provider) == 0 ||
      provider.dli_fname == nullptr || provider.dli_fbase == nullptr)
    return fail(failure, Error::AnchorUnavailable);
  struct stat st{};
  const size_t path_size = strnlen(provider.dli_fname, PATH_MAX);
  if (path_size == 0 || path_size == PATH_MAX || provider.dli_fname[0] != '/')
    return fail(failure, Error::IdentityMismatch);
  // Bionic dladdr reports the object's real path; maps verify its identity.
  if (stat(provider.dli_fname, &st) != 0)
    return fail(failure, Error::AnchorUnavailable, errno);
  if (!S_ISREG(st.st_mode) || st.st_size <= 0)
    return fail(failure, Error::IdentityMismatch);
  const std::string_view name(provider.dli_fname, path_size);
  if (name.substr(name.find_last_of('/') + 1) != "libc.so")
    return fail(failure, Error::IdentityMismatch);
  const Object expected{identity_of(st), std::string(name), 0,
                        reinterpret_cast<uintptr_t>(provider.dli_fbase)};
  context.reference = code_address(reinterpret_cast<uintptr_t>(libc_symbol));
  bool found_reference = false;
  for (const auto &map : maps) {
    if (context.reference < map.start || context.reference >= map.end)
      continue;
    const uint64_t relative = context.reference - map.start;
    if (!expected.identity.matches(map) || (map.perms & PROT_EXEC) == 0 ||
        relative > UINT64_MAX - map.offset)
      return fail(failure, Error::IdentityMismatch);
    context.reference_offset = map.offset + relative;
    found_reference = true;
    break;
  }
  if (!found_reference)
    return fail(failure, Error::IdentityMismatch);
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || (page & (page - 1)) != 0)
    return fail(failure, Error::InvalidElf);
  Scan scan{maps,
            expected,
            static_cast<uint64_t>(st.st_size),
            static_cast<uintptr_t>(page),
            context.reference,
            context,
            failure};
  const int result = api.iterate(collect_libc, &scan);
  if (!scan.valid)
    return false;
  if (result != 0 || scan.candidates != 1)
    return fail(failure, Error::AnchorUnavailable);
  Dl_info found{};
  if (api.address_info(reinterpret_cast<const void *>(context.anchor),
                       &found) == 0 ||
      reinterpret_cast<uintptr_t>(found.dli_fbase) != context.libc.base ||
      found.dli_fname == nullptr)
    return fail(failure, Error::IdentityMismatch);
  if (stat(found.dli_fname, &st) != 0)
    return fail(failure, Error::IdentityMismatch, errno);
  if (!same_identity(identity_of(st), expected.identity))
    return fail(failure, Error::IdentityMismatch);
  failure.error = Error::None;
  return true;
}

bool validate_context(const CallerContext &context, const Maps &maps,
                      Failure &failure) {
  failure.stage = Stage::Revalidate;
  if (maps.empty())
    return fail(failure, Error::MapsUnavailable);
  size_t index = 0;
  for (const uintptr_t entry :
       {reinterpret_cast<uintptr_t>(context.api.open),
        reinterpret_cast<uintptr_t>(context.api.iterate),
        reinterpret_cast<uintptr_t>(context.api.address_info),
        reinterpret_cast<uintptr_t>(context.api.lookup),
        reinterpret_cast<uintptr_t>(context.api.lookup_version)}) {
    if (!mapped_range(maps, code_address(entry), instruction_size(entry),
                      PROT_EXEC, &context.api.linker.identity,
                      &context.api.file_offsets[index++]))
      return fail(failure, Error::IdentityMismatch);
  }
  if (context.anchor == 0 || context.anchor >= context.segment_end ||
      !mapped_range(maps, context.reference, 1, PROT_EXEC,
                    &context.libc.identity, &context.reference_offset) ||
      !mapped_range(maps, context.anchor, 1, PROT_READ, &context.libc.identity,
                    &context.file_offset))
    return fail(failure, Error::IdentityMismatch);
  failure.error = Error::None;
  return true;
}

bool prepare_caller(CallerContext &context, Failure &failure) {
  const auto maps = read_maps();
  if (maps.empty())
    return fail(failure, Error::MapsUnavailable);
  LoaderApi api;
  return resolve_loader(maps, api, failure) &&
         find_caller(api, maps, reinterpret_cast<const void *>(getpid), context,
                     failure) &&
         validate_context(context, read_maps(), failure);
}

LoadResult load(const char *path, int flags, const android_dlextinfo &ext) {
  LoadResult result;
  if (!prepare_caller(result.context, result.failure))
    return result;
  result.failure.stage = Stage::Load;
  dlerror();
  errno = 0;
  result.handle = result.context.api.open(
      path, flags, &ext, reinterpret_cast<const void *>(result.context.anchor));
  if (result.handle == nullptr) {
    const int saved_errno = errno;
    const char *error = dlerror();
    result.loader_error = error != nullptr ? error : "no linker error";
    fail(result.failure, Error::LoaderFailure, saved_errno);
  }
  return result;
}

const char *stage_name(Stage stage) {
  switch (stage) {
  case Stage::Linker:
    return "linker";
  case Stage::Anchor:
    return "anchor";
  case Stage::Revalidate:
    return "revalidate";
  case Stage::Load:
    return "load";
  }
  return "unknown";
}

const char *error_name(Error error) {
  switch (error) {
  case Error::None:
    return "none";
  case Error::MapsUnavailable:
    return "maps-unavailable";
  case Error::InvalidElf:
    return "invalid-elf";
  case Error::IdentityMismatch:
    return "identity-mismatch";
  case Error::SymbolUnavailable:
    return "symbol-unavailable";
  case Error::SymbolAmbiguous:
    return "symbol-ambiguous";
  case Error::AnchorUnavailable:
    return "anchor-unavailable";
  case Error::AnchorAmbiguous:
    return "anchor-ambiguous";
  case Error::LoaderFailure:
    return "loader-failure";
  }
  return "unknown";
}

} // namespace yukizygisk::native_bionic
