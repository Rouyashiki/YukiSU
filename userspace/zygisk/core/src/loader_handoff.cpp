#include "loader_handoff.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace yukizygisk::handoff {
namespace {

struct Map {
  uintptr_t begin;
  uintptr_t end;
  uint64_t offset;
  uint64_t inode;
  unsigned major;
  unsigned minor;
  bool readable;
};
using Maps = std::vector<Map>;

bool same_file(const Map &a, const Map &b) {
  return a.inode != 0 && a.inode == b.inode && a.major == b.major &&
         a.minor == b.minor;
}

Maps read_maps() {
  Maps maps;
  FILE *stream = fopen("/proc/self/maps", "re");
  if (stream == nullptr)
    return maps;
  char *line = nullptr;
  size_t capacity = 0;
  bool valid = true;
  while (getline(&line, &capacity, stream) >= 0) {
    unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
    unsigned major = 0, minor = 0;
    char permissions[5]{};
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu", &begin, &end, permissions,
               &offset, &major, &minor, &inode) != 7 ||
        begin >= end || end > UINTPTR_MAX ||
        (!maps.empty() && maps.back().end > begin)) {
      valid = false;
      break;
    }
    maps.push_back({static_cast<uintptr_t>(begin), static_cast<uintptr_t>(end),
                    offset, inode, major, minor, permissions[0] == 'r'});
  }
  valid &= ferror(stream) == 0;
  free(line);
  valid &= fclose(stream) == 0;
  if (!valid)
    maps.clear();
  return maps;
}

bool add(uintptr_t base, uint64_t offset, uintptr_t &value) {
  if (offset > UINTPTR_MAX - base)
    return false;
  value = base + static_cast<uintptr_t>(offset);
  return true;
}

bool readable(const Maps &maps, uintptr_t begin, size_t size) {
  uintptr_t end = 0;
  if (size == 0 || !add(begin, size, end))
    return false;
  for (const auto &map : maps) {
    if (map.end <= begin)
      continue;
    if (map.begin > begin || !map.readable)
      return false;
    if (map.end >= end)
      return true;
    begin = map.end;
  }
  return false;
}

struct Image {
  uintptr_t bias = 0;
  uintptr_t end = 0;
  std::vector<ElfW(Phdr)> segments;
};

bool image_at(const Maps &maps, uintptr_t base, Image &image) {
  if (!readable(maps, base, sizeof(ElfW(Ehdr))))
    return false;
  ElfW(Ehdr) header;
  memcpy(&header, reinterpret_cast<const void *>(base), sizeof(header));
#if defined(__aarch64__)
  constexpr auto machine = EM_AARCH64;
#elif defined(__arm__)
  constexpr auto machine = EM_ARM;
#else
  constexpr auto machine = EM_X86_64;
#endif
  if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] !=
          (sizeof(uintptr_t) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      header.e_ident[EI_DATA] != ELFDATA2LSB ||
      header.e_ident[EI_VERSION] != EV_CURRENT ||
      header.e_version != EV_CURRENT || header.e_type != ET_DYN ||
      header.e_machine != machine || header.e_ehsize != sizeof(header) ||
      header.e_phentsize != sizeof(ElfW(Phdr)) || header.e_phnum == 0 ||
      header.e_phnum == PN_XNUM)
    return false;
  uintptr_t table = 0;
  const size_t bytes = size_t{header.e_phnum} * sizeof(ElfW(Phdr));
  if (!add(base, header.e_phoff, table) || !readable(maps, table, bytes))
    return false;
  image.segments.resize(header.e_phnum);
  memcpy(image.segments.data(), reinterpret_cast<const void *>(table), bytes);
  bool found_header = false;
  for (const auto &segment : image.segments) {
    if (segment.p_type == PT_LOAD && segment.p_offset == 0 &&
        segment.p_filesz >= sizeof(header)) {
      // Non-zero minimum vaddr needs a different reservation/bias contract.
      if (found_header || segment.p_vaddr != 0)
        return false;
      found_header = true;
    }
  }
  if (!found_header)
    return false;
  image.bias = base;
  image.end = base;
  for (const auto &segment : image.segments) {
    if (segment.p_type != PT_LOAD)
      continue;
    uintptr_t begin = 0, end = 0;
    if (segment.p_filesz > segment.p_memsz ||
        !add(base, segment.p_vaddr, begin) || !add(begin, segment.p_memsz, end))
      return false;
    image.end = std::max(image.end, end);
  }
  return image.end > image.bias && table >= base && table <= image.end &&
         bytes <= image.end - table;
}

bool contains(const Image &image, uintptr_t address, size_t bytes) {
  if (address < image.bias || address >= image.end ||
      bytes > image.end - address)
    return false;
  for (const auto &segment : image.segments) {
    if (segment.p_type != PT_LOAD)
      continue;
    const uintptr_t begin = image.bias + segment.p_vaddr;
    if (address >= begin && address - begin < segment.p_memsz &&
        bytes <= segment.p_memsz - (address - begin))
      return true;
  }
  return false;
}

bool loader_image(const Maps &maps, uintptr_t reference, Image &image,
                  Map &identity) {
  bool found = false;
  for (const auto &map : maps) {
    if (reference >= map.begin && reference < map.end && map.inode != 0) {
      identity = map;
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  unsigned candidates = 0;
  for (const auto &map : maps) {
    if (map.offset != 0 || !same_file(map, identity))
      continue;
    Image candidate;
    if (image_at(maps, map.begin, candidate) &&
        contains(candidate, reference, 1)) {
      image = std::move(candidate);
      ++candidates;
    }
  }
  if (candidates != 1)
    return false;
  const long page = sysconf(_SC_PAGESIZE);
  uintptr_t rounded = 0;
  if (page <= 0 || (page & (page - 1)) != 0 ||
      !add(image.end, static_cast<uintptr_t>(page - 1), rounded))
    return false;
  image.end = rounded & ~static_cast<uintptr_t>(page - 1);
  return true;
}

Result pointer(const Maps &maps, const Image &loader, const Map &identity,
               uintptr_t slot, uintptr_t target) {
  if (target == 0)
    return {true, "null weak pointer", slot, target};
  if (target >= loader.bias && target < loader.end)
    return {false, "reference into loader image", slot, target};
  for (const auto &map : maps) {
    if (target >= map.begin && target < map.end) {
      if (same_file(map, identity))
        return {false, "reference into loader file", slot, target};
      return {true, "mapped pointer", slot, target};
    }
  }
  return {false, "unmapped relocation target", slot, target};
}

constexpr auto kStandardRelr = 36;
constexpr auto kStandardRelrSize = 35;
constexpr auto kStandardRelrEntry = 37;
constexpr auto kAndroidRelr = 0x6fffe000;
constexpr auto kAndroidRelrSize = 0x6fffe001;
constexpr auto kAndroidRelrEntry = 0x6fffe003;
constexpr auto kAndroidRel = 0x6000000f;
constexpr auto kAndroidRela = 0x60000011;

#if defined(__LP64__)
using Relocation = ElfW(Rela);
constexpr auto kRel = DT_RELA;
constexpr auto kRelSize = DT_RELASZ;
constexpr auto kRelEntry = DT_RELAENT;
#else
using Relocation = ElfW(Rel);
constexpr auto kRel = DT_REL;
constexpr auto kRelSize = DT_RELSZ;
constexpr auto kRelEntry = DT_RELENT;
#endif

bool pointer_relocation(uint64_t info) {
#if defined(__aarch64__)
  const auto type = ELF64_R_TYPE(info);
  return type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
         type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_RELATIVE;
#elif defined(__arm__)
  const auto type = ELF32_R_TYPE(info);
  return type == R_ARM_ABS32 || type == R_ARM_GLOB_DAT ||
         type == R_ARM_JUMP_SLOT || type == R_ARM_RELATIVE;
#else
  const auto type = ELF64_R_TYPE(info);
  return type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
         type == R_X86_64_JUMP_SLOT || type == R_X86_64_RELATIVE;
#endif
}

Result verify_snapshot(const Maps &maps, uintptr_t core, size_t size,
                       uintptr_t loader_reference,
                       const uintptr_t *capabilities, size_t count,
                       bool system_tls = false) {
  Image self, loader;
  Map identity{};
  if (!image_at(maps, core, self) || size == 0 || size > UINTPTR_MAX - core ||
      self.end > core + size ||
      !loader_image(maps, loader_reference, loader, identity))
    return {false, "image bounds or loader identity unavailable"};
  for (size_t i = 0; i < count; ++i) {
    auto result = pointer(maps, loader, identity, 0, capabilities[i]);
    if (!result.complete)
      return result;
  }
  uintptr_t dynamic = 0;
  size_t dynamic_size = 0;
  for (const auto &segment : self.segments) {
    if (segment.p_type == PT_DYNAMIC) {
      if (dynamic != 0 || !add(core, segment.p_vaddr, dynamic))
        return {false, "invalid dynamic segment"};
      dynamic_size = segment.p_memsz;
    }
    if (!system_tls && segment.p_type == PT_TLS && segment.p_memsz != 0)
      return {false, "anonymous TLS ownership is not transferable", 0, 0, true};
  }
  if (dynamic_size == 0 || dynamic_size % sizeof(ElfW(Dyn)) != 0 ||
      !contains(self, dynamic, dynamic_size) ||
      !readable(maps, dynamic, dynamic_size))
    return {false, "invalid dynamic segment bounds"};
  uintptr_t tables[3]{};
  size_t sizes[3]{};
  bool size_seen[3]{};
  bool terminated = false;
  for (size_t offset = 0; offset < dynamic_size; offset += sizeof(ElfW(Dyn))) {
    ElfW(Dyn) entry;
    memcpy(&entry, reinterpret_cast<const void *>(dynamic + offset),
           sizeof(entry));
    if (entry.d_tag == DT_NULL) {
      terminated = true;
      break;
    }
    const auto tag = entry.d_tag;
    if (tag == kRel || tag == DT_JMPREL || tag == kStandardRelr ||
        tag == kAndroidRelr) {
      const size_t index = tag == kRel ? 0 : (tag == DT_JMPREL ? 1 : 2);
      if (tables[index] != 0 || !add(core, entry.d_un.d_ptr, tables[index]))
        return {false, "invalid relocation table"};
    } else if (tag == kRelSize || tag == DT_PLTRELSZ ||
               tag == kStandardRelrSize || tag == kAndroidRelrSize) {
      const size_t index = tag == kRelSize ? 0 : (tag == DT_PLTRELSZ ? 1 : 2);
      if (size_seen[index])
        return {false, "duplicate relocation size"};
      size_seen[index] = true;
      sizes[index] = entry.d_un.d_val;
    } else if ((tag == kRelEntry && entry.d_un.d_val != sizeof(Relocation)) ||
               (tag == DT_PLTREL && entry.d_un.d_val != kRel) ||
               ((tag == kStandardRelrEntry || tag == kAndroidRelrEntry) &&
                entry.d_un.d_val != sizeof(uintptr_t))) {
      return {false, "unsupported relocation entry format"};
    } else if (tag == DT_REL || tag == DT_RELA || tag == kAndroidRel ||
               tag == kAndroidRela) {
      // APS2 and the other ABI's format must not silently skip references.
      return {false, "unsupported relocation encoding"};
    }
  }
  if (!terminated || (sizes[0] == 0 && sizes[1] == 0 && sizes[2] == 0))
    return {false, "missing relocation metadata"};
  const auto check_slot = [&](uintptr_t offset) -> Result {
    uintptr_t slot = 0, value = 0;
    if (!add(core, offset, slot) || slot % alignof(uintptr_t) != 0 ||
        !contains(self, slot, sizeof(value)) ||
        !readable(maps, slot, sizeof(value)))
      return {false, "invalid relocation slot", slot};
    memcpy(&value, reinterpret_cast<const void *>(slot), sizeof(value));
    return pointer(maps, loader, identity, slot, value);
  };
  for (size_t table = 0; table < 3; ++table) {
    if (tables[table] != 0 && !size_seen[table])
      return {false, "missing relocation table size"};
    if (sizes[table] == 0)
      continue;
    const size_t entry_size =
        table == 2 ? sizeof(uintptr_t) : sizeof(Relocation);
    if (sizes[table] % entry_size != 0 ||
        !contains(self, tables[table], sizes[table]) ||
        !readable(maps, tables[table], sizes[table]))
      return {false, "invalid relocation table bounds"};
    uintptr_t next = 0;
    bool relr_base_seen = false;
    for (size_t offset = 0; offset < sizes[table]; offset += entry_size) {
      if (table == 2) {
        uintptr_t encoded = 0;
        memcpy(&encoded, reinterpret_cast<const void *>(tables[table] + offset),
               sizeof(encoded));
        if ((encoded & 1) == 0) {
          const auto result = check_slot(encoded);
          if (!result.complete)
            return result;
          if (!add(encoded, sizeof(uintptr_t), next))
            return {false, "RELR address overflow"};
          relr_base_seen = true;
        } else {
          if (!relr_base_seen)
            return {false, "RELR bitmap without base"};
          constexpr size_t bits = (sizeof(uintptr_t) * 8) - 1;
          for (size_t bit = 0; bit < bits; ++bit) {
            if (((encoded >> 1) & (uintptr_t{1} << bit)) == 0)
              continue;
            uintptr_t slot_offset = 0;
            if (!add(next, static_cast<uint64_t>(bit) * sizeof(uintptr_t),
                     slot_offset))
              return {false, "RELR address overflow"};
            const auto result = check_slot(slot_offset);
            if (!result.complete)
              return result;
          }
          if (!add(next, static_cast<uint64_t>(bits) * sizeof(uintptr_t), next))
            return {false, "RELR address overflow"};
        }
        continue;
      }
      Relocation relocation;
      memcpy(&relocation,
             reinterpret_cast<const void *>(tables[table] + offset),
             sizeof(relocation));
      if (relocation.r_info == 0)
        continue;
#if defined(__aarch64__)
      if (system_tls && ELF64_R_SYM(relocation.r_info) == 0 &&
          ELF64_R_TYPE(relocation.r_info) == R_AARCH64_TLSDESC) {
        uintptr_t descriptor = 0;
        if (!add(core, relocation.r_offset, descriptor) ||
            !contains(self, descriptor, 2 * sizeof(uintptr_t)) ||
            !readable(maps, descriptor, 2 * sizeof(uintptr_t)))
          return {false, "invalid Bionic TLS descriptor", descriptor};
        const auto result = check_slot(relocation.r_offset);
        if (!result.complete)
          return result;
        continue;
      }
#elif defined(__arm__)
      if (system_tls && ELF32_R_SYM(relocation.r_info) == 0 &&
          ELF32_R_TYPE(relocation.r_info) == R_ARM_TLS_DTPMOD32) {
        uintptr_t slot = 0, module = 0;
        if (!add(core, relocation.r_offset, slot) ||
            !contains(self, slot, sizeof(module)) ||
            !readable(maps, slot, sizeof(module)))
          return {false, "invalid Bionic TLS module slot", slot};
        memcpy(&module, reinterpret_cast<void *>(slot), sizeof(module));
        if (module == 0)
          return {false, "unbound Bionic TLS module", slot};
        continue;
      }
#endif
      if (!pointer_relocation(relocation.r_info))
        return {false, "unsupported relocation type"};
      const auto result = check_slot(relocation.r_offset);
      if (!result.complete)
        return result;
    }
  }
  return {true, "relocations and capabilities handed off"};
}

} // namespace

Result verify(uintptr_t core, size_t size, uintptr_t loader,
              const uintptr_t *capabilities, size_t count) {
  if (count != 0 && capabilities == nullptr)
    return {false, "missing capability list"};
  return verify_snapshot(read_maps(), core, size, loader, capabilities, count);
}

Result verify_loaded(uintptr_t core_reference, uintptr_t loader,
                     const uintptr_t *capabilities, size_t count) {
  if (count != 0 && capabilities == nullptr)
    return {false, "missing capability list"};
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(core_reference), &info) == 0)
    return {false, "Bionic core identity unavailable"};
  const auto maps = read_maps();
  Image self;
  if (!image_at(maps, reinterpret_cast<uintptr_t>(info.dli_fbase), self) ||
      !contains(self, core_reference, 1))
    return {false, "Bionic core bounds unavailable"};
  return verify_snapshot(maps, self.bias, self.end - self.bias, loader,
                         capabilities, count, true);
}

} // namespace yukizygisk::handoff

namespace {
void **g_bootstrap_dependencies = nullptr;
size_t g_dependency_count = 0;
uintptr_t g_anonymous_base = 0;
size_t g_anonymous_span = 0;
bool g_dependencies_transferred = false;
bool g_retirement_ready = false;
void *g_system_core_handle = nullptr;
void *g_system_loader_handle = nullptr;
void core_identity_anchor() {}
} // namespace

bool yukizygisk::handoff::retirement_ready() { return g_retirement_ready; }

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_finish_handoff(bool dependencies_required) {
  if (g_retirement_ready ||
      (dependencies_required && !g_dependencies_transferred))
    return false;
  g_retirement_ready = true;
  return true;
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_bootstrap_take_dependencies(void *const *handles, size_t count) {
  if (g_dependencies_transferred || (count != 0 && handles == nullptr) ||
      count > SIZE_MAX / sizeof(void *))
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (handles[i] == nullptr)
      return false;
  }
  void **owned = nullptr;
  if (count != 0) {
    owned = static_cast<void **>(malloc(count * sizeof(void *)));
    if (owned == nullptr)
      return false;
    std::copy_n(handles, count, owned);
  }
  // Transfer exactly the references acquired by the first stage.
  g_bootstrap_dependencies = owned;
  g_dependency_count = count;
  g_dependencies_transferred = true;
  return true;
}

extern "C" [[gnu::visibility("default")]] bool
zygisk_adopt_bootstrap_handles(void *core, void *loader) {
  if (core == nullptr || g_system_core_handle != nullptr)
    return false;
  Dl_info self{}, found{};
  const void *entry = dlsym(core, "zygisk_bootstrap_finish_handoff");
  if (entry == nullptr ||
      dladdr(reinterpret_cast<void *>(core_identity_anchor), &self) == 0 ||
      dladdr(entry, &found) == 0 || self.dli_fbase != found.dli_fbase)
    return false;
  if (loader != nullptr) {
    const void *bootstrap = dlsym(loader, "yuki_bootstrap");
    if (bootstrap == nullptr || dladdr(bootstrap, &found) == 0 ||
        found.dli_fbase == self.dli_fbase)
      return false;
  }
  g_system_core_handle = core;
  g_system_loader_handle = loader;
  return true;
}

bool yukizygisk::handoff::adopt_anonymous_loader(uintptr_t base, size_t span,
                                                 void *loader) {
  const uintptr_t anchor = reinterpret_cast<uintptr_t>(core_identity_anchor);
  if (base == 0 || span == 0 || span > UINTPTR_MAX - base || anchor < base ||
      anchor - base >= span || loader == nullptr ||
      g_system_loader_handle != nullptr || g_system_core_handle != nullptr ||
      g_anonymous_base != 0)
    return false;
  const void *bootstrap = dlsym(loader, "yuki_bootstrap");
  Dl_info provider{};
  if (bootstrap == nullptr || dladdr(bootstrap, &provider) == 0 ||
      (reinterpret_cast<uintptr_t>(bootstrap) >= base &&
       reinterpret_cast<uintptr_t>(bootstrap) - base < span))
    return false;
  g_anonymous_base = base;
  g_anonymous_span = span;
  g_system_loader_handle = loader;
  return true;
}

bool yukizygisk::handoff::anonymous_image_matches(uintptr_t base, size_t span) {
  return base != 0 && base == g_anonymous_base && span == g_anonymous_span;
}

bool yukizygisk::handoff::release_dependencies() {
  if (!g_dependencies_transferred)
    return false;
  for (size_t i = g_dependency_count; i != 0; --i) {
    void *&handle = g_bootstrap_dependencies[i - 1];
    if (handle != nullptr) {
      if (dlclose(handle) != 0)
        return false;
      handle = nullptr;
    }
  }
  // The POD handoff retains this allocation across retirement retries.
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  free(static_cast<void *>(g_bootstrap_dependencies));
  g_bootstrap_dependencies = nullptr;
  g_dependency_count = 0;
  return true;
}

bool yukizygisk::handoff::owns_core_reference() {
  return g_system_core_handle != nullptr;
}
void *yukizygisk::handoff::take_core_reference() {
  void *handle = g_system_core_handle;
  g_system_core_handle = nullptr;
  return handle;
}
bool yukizygisk::handoff::close_loader_reference() {
  if (g_system_loader_handle == nullptr)
    return false;
  if (dlclose(g_system_loader_handle) != 0)
    return false;
  g_system_loader_handle = nullptr;
  return true;
}
