#pragma once

#include "ihook_lifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

#if defined(__arm__) && defined(__ANDROID__)
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace yuki::ihook {

namespace sync_detail {

enum class Backend : uint8_t { Native, Tango, Unknown };

struct Mapping {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  int protection = 0;
};

[[nodiscard]] inline bool outside(const Mapping &mapping, uintptr_t pc) {
  pc &= ~uintptr_t{1};
  return pc < mapping.begin || pc >= mapping.end;
}

[[nodiscard]] inline bool
transition_range(const Mapping &mapping, uintptr_t address, size_t size,
                 uintptr_t page, uintptr_t executing, uintptr_t resume,
                 uintptr_t syscall_pc, uintptr_t *page_begin) {
  if (page_begin == nullptr || page == 0 || size == 0 ||
      address > UINTPTR_MAX - size || mapping.begin >= mapping.end)
    return false;
  const uintptr_t begin = address - address % page;
  if ((mapping.protection & (PROT_READ | PROT_EXEC)) !=
          (PROT_READ | PROT_EXEC) ||
      begin < mapping.begin || begin > UINTPTR_MAX - page ||
      begin + page > mapping.end || address + size > begin + page ||
      !outside(mapping, executing) || !outside(mapping, resume) ||
      !outside(mapping, syscall_pc))
    return false;
  *page_begin = begin;
  return true;
}

#if defined(__arm__) && defined(__ANDROID__)
[[nodiscard]] inline bool supported_translator(int fd, const Elf64_Ehdr &header,
                                               uint64_t file_size) {
  constexpr uint8_t supported_id[20] = {
      0x34, 0x44, 0x9e, 0xa9, 0xd1, 0xce, 0x2d, 0xf9, 0xfe, 0x0b,
      0xd9, 0x95, 0x42, 0x73, 0x05, 0x73, 0xbe, 0x95, 0xc6, 0xf8};
  if (file_size > INT64_MAX || header.e_phentsize != sizeof(Elf64_Phdr) ||
      header.e_phnum > 128 || header.e_phoff > file_size ||
      uint64_t{header.e_phnum} * sizeof(Elf64_Phdr) >
          file_size - header.e_phoff)
    return false;
  for (unsigned index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr segment{};
    if (pread64(fd, &segment, sizeof(segment),
                static_cast<off64_t>(header.e_phoff +
                                     uint64_t{index} * sizeof(segment))) !=
        sizeof(segment))
      return false;
    if (segment.p_type != PT_NOTE)
      continue;
    if (segment.p_filesz > 4096 || segment.p_offset > file_size ||
        segment.p_filesz > file_size - segment.p_offset)
      return false;
    uint8_t notes[4096];
    if (pread64(fd, notes, segment.p_filesz,
                static_cast<off64_t>(segment.p_offset)) !=
        static_cast<ssize_t>(segment.p_filesz))
      return false;
    size_t offset = 0;
    while (segment.p_filesz - offset >= sizeof(Elf64_Nhdr)) {
      Elf64_Nhdr note{};
      memcpy(&note, notes + offset, sizeof(note));
      offset += sizeof(note);
      if (note.n_namesz > 4096 || note.n_descsz > 4096)
        return false;
      const size_t name_size = (note.n_namesz + 3U) & ~size_t{3};
      const size_t data_size = (note.n_descsz + 3U) & ~size_t{3};
      if (name_size + data_size > segment.p_filesz - offset)
        return false;
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
          note.n_descsz == sizeof(supported_id) &&
          memcmp(notes + offset, "GNU", 4) == 0 &&
          memcmp(notes + offset + name_size, supported_id,
                 sizeof(supported_id)) == 0)
        return true;
      offset += name_size + data_size;
    }
  }
  return false;
}

[[nodiscard]] inline Backend inspect(uintptr_t address, size_t size,
                                     Mapping *mapping) {
  FILE *maps = fopen("/proc/self/./maps", "re");
  if (maps == nullptr)
    return Backend::Unknown;
  constexpr char translator[] = "/system_ext/bin/tango_translator";
  struct stat identity{};
  Elf64_Ehdr header{};
  const int fd = open(translator, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const bool have_identity =
      fd >= 0 && fstat(fd, &identity) == 0 && S_ISREG(identity.st_mode) &&
      identity.st_uid == 0 &&
      pread(fd, &header, sizeof(header), 0) == sizeof(header) &&
      memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
      header.e_ident[EI_CLASS] == ELFCLASS64 &&
      header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_machine == EM_AARCH64;
  const bool supported =
      have_identity && identity.st_size >= 0 &&
      supported_translator(fd, header, static_cast<uint64_t>(identity.st_size));
  if (fd >= 0)
    close(fd);
  struct stat executable_identity{};
  Elf64_Ehdr executable_header{};
  const int executable = open("/proc/self/./exe", O_RDONLY | O_CLOEXEC);
  const bool executable_read =
      executable >= 0 && fstat(executable, &executable_identity) == 0 &&
      pread(executable, &executable_header, sizeof(executable_header), 0) ==
          sizeof(executable_header) &&
      memcmp(executable_header.e_ident, ELFMAG, SELFMAG) == 0;
  if (executable >= 0)
    close(executable);
  const bool interpreter = executable_read && have_identity &&
                           executable_identity.st_dev == identity.st_dev &&
                           executable_identity.st_ino == identity.st_ino &&
                           executable_header.e_ident[EI_CLASS] == ELFCLASS64 &&
                           executable_header.e_machine == EM_AARCH64;
  const bool native = executable_read &&
                      executable_header.e_ident[EI_CLASS] == ELFCLASS32 &&
                      executable_header.e_machine == EM_ARM;
  bool translated = false;
  bool host_executable = false;
  bool ambiguous = false;
  bool found = false;
  char line[8192];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
    unsigned device_major = 0, device_minor = 0;
    char permissions[5] = {};
    int path_offset = 0;
    // These bounded fields come directly from the kernel's proc maps formatter.
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &begin, &end,
               permissions, &offset, &device_major, &device_minor, &inode,
               &path_offset) != 7 ||
        begin >= end) {
      ambiguous = true;
      continue;
    }
    const char *path = line + path_offset;
    host_executable |= begin > UINT32_MAX && permissions[2] == 'x';
    if (strncmp(path, translator, sizeof(translator) - 1) == 0) {
      const bool exact = path[sizeof(translator) - 1] == '\n' ||
                         path[sizeof(translator) - 1] == '\0';
      const bool matched = have_identity && inode == identity.st_ino &&
                           device_major == major(identity.st_dev) &&
                           device_minor == minor(identity.st_dev);
      if (!exact || !matched || begin <= UINT32_MAX)
        ambiguous = true;
      else if (permissions[2] == 'x')
        translated = true;
    }
    if (address >= begin && address + size <= end && end <= UINTPTR_MAX) {
      mapping->begin = static_cast<uintptr_t>(begin);
      mapping->end = static_cast<uintptr_t>(end);
      mapping->protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                            (permissions[1] == 'w' ? PROT_WRITE : 0) |
                            (permissions[2] == 'x' ? PROT_EXEC : 0);
      found = permissions[3] == 'p';
    }
  }
  const bool complete = !ferror(maps);
  fclose(maps);
  if (!complete || ambiguous || !found || (host_executable && !translated) ||
      (translated && (!supported || !interpreter)) || (!translated && !native))
    return Backend::Unknown;
  return translated ? Backend::Tango : Backend::Native;
}

[[nodiscard]] inline bool one_thread() {
  DIR *tasks = opendir("/proc/self/task");
  if (tasks == nullptr)
    return false;
  unsigned count = 0;
  while (const dirent *entry = readdir(tasks)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
      ++count;
  }
  closedir(tasks);
  return count == 1;
}

using lifecycle_detail::raw_call;

[[nodiscard]] inline bool raw_one_thread() {
  constexpr char task_path[] = "/proc/self/task";
  const long fd = raw_call(322, static_cast<uintptr_t>(-100),
                           reinterpret_cast<uintptr_t>(task_path),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return false;
  unsigned count = 0;
  bool complete = false;
  for (;;) {
    alignas(uint64_t) uint8_t entries[1024];
    const long length = raw_call(217, fd, reinterpret_cast<uintptr_t>(entries),
                                 sizeof(entries));
    if (length == 0) {
      complete = true;
      break;
    }
    if (length < 0 || static_cast<size_t>(length) > sizeof(entries))
      break;
    size_t offset = 0;
    bool valid = true;
    while (offset < static_cast<size_t>(length)) {
      if (static_cast<size_t>(length) - offset < 20) {
        valid = false;
        break;
      }
      const size_t record =
          entries[offset + 16] | (size_t{entries[offset + 17]} << 8);
      if (record < 20 || record > static_cast<size_t>(length) - offset) {
        valid = false;
        break;
      }
      const uint8_t initial = entries[offset + 19];
      if (initial >= '0' && initial <= '9')
        ++count;
      offset += record;
    }
    if (!valid || count > 1)
      break;
  }
  const long closed = raw_call(6, fd, 0, 0);
  return complete && closed == 0 && count == 1;
}

#endif

} // namespace sync_detail

struct InstructionSyncPlan {
  sync_detail::Backend backend = sync_detail::Backend::Unknown;
  uintptr_t address = 0;
  size_t size = 0;
  uintptr_t page_begin = 0;
  uintptr_t page_size = 0;
  int protection = 0;
  bool valid = false;
};

[[nodiscard]] inline bool
instruction_sync_still_supported(const InstructionSyncPlan &plan) {
  if (!plan.valid)
    return false;
#if defined(__arm__) && defined(__ANDROID__)
  return plan.backend != sync_detail::Backend::Tango ||
         sync_detail::raw_one_thread();
#else
  return true;
#endif
}

[[nodiscard]] SyncResult
synchronize_instruction_view(const InstructionSyncPlan &plan,
                             SyncOperation operation);

[[nodiscard]] __attribute__((noinline)) inline bool
prepare_instruction_sync(uintptr_t address, size_t size,
                         InstructionSyncPlan *plan) {
  if (plan == nullptr || size == 0 || address > UINTPTR_MAX - size)
    return false;
  *plan = {};
  plan->address = address;
  plan->size = size;
#if defined(__arm__) && defined(__ANDROID__)
  sync_detail::Mapping mapping{};
  plan->backend = sync_detail::inspect(address, size, &mapping);
  if (plan->backend == sync_detail::Backend::Unknown)
    return false;
  if (plan->backend == sync_detail::Backend::Tango) {
    plan->page_size = static_cast<uintptr_t>(getpagesize());
    plan->protection = mapping.protection;
    const auto synchronizer =
        static_cast<SyncResult (*)(const InstructionSyncPlan &, SyncOperation)>(
            synchronize_instruction_view);
    if (!sync_detail::transition_range(
            mapping, address, size, plan->page_size,
            reinterpret_cast<uintptr_t>(synchronizer),
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
            reinterpret_cast<uintptr_t>(sync_detail::raw_call),
            &plan->page_begin) ||
        !sync_detail::one_thread())
      return false;
  }
#else
  plan->backend = sync_detail::Backend::Native;
#endif
  plan->valid = true;
  return true;
}

[[nodiscard]] __attribute__((noinline)) inline SyncResult
synchronize_instruction_view(const InstructionSyncPlan &plan,
                             SyncOperation operation) {
  if (!plan.valid || plan.size == 0 || plan.address > UINTPTR_MAX - plan.size)
    return SyncResult::Indeterminate;
#if defined(__arm__) && defined(__ANDROID__)
  if (plan.backend == sync_detail::Backend::Tango &&
      operation != SyncOperation::Prepare) {
    uint64_t previous = 0;
    const uint64_t blocked = UINT64_MAX;
    if (sync_detail::raw_call(175, 0, reinterpret_cast<uintptr_t>(&blocked),
                              reinterpret_cast<uintptr_t>(&previous),
                              sizeof(blocked)) != 0)
      return SyncResult::Indeterminate;
    const long refreshed = sync_detail::raw_call(
        125, plan.page_begin, plan.page_size, plan.protection);
    // This translator moves refreshed RX regions into its mutable-code index.
    // cacheflush then removes the indexed translations and incoming links.
    const long flushed =
        refreshed == 0 ? sync_detail::raw_call(0x0f0002, plan.address,
                                               plan.address + plan.size, 0)
                       : -1;
    const long unblocked = sync_detail::raw_call(
        175, 2, reinterpret_cast<uintptr_t>(&previous), 0, sizeof(previous));
    return refreshed == 0 && flushed == 0 && unblocked == 0
               ? SyncResult::Confirmed
               : SyncResult::Indeterminate;
  }
  return sync_detail::raw_call(0x0f0002, plan.address, plan.address + plan.size,
                               0) == 0
             ? SyncResult::Confirmed
             : SyncResult::Indeterminate;
#else
  (void)operation;
  __builtin___clear_cache(reinterpret_cast<char *>(plan.address),
                          reinterpret_cast<char *>(plan.address + plan.size));
  return SyncResult::Confirmed;
#endif
}

[[nodiscard]] inline SyncResult
synchronize_instruction_view(uintptr_t address, size_t size,
                             SyncOperation operation) {
  if (operation != SyncOperation::Prepare || size == 0 ||
      address > UINTPTR_MAX - size)
    return SyncResult::Indeterminate;
  InstructionSyncPlan plan{};
  plan.address = address;
  plan.size = size;
  plan.backend = sync_detail::Backend::Native;
  plan.valid = true;
  return synchronize_instruction_view(plan, operation);
}

} // namespace yuki::ihook
