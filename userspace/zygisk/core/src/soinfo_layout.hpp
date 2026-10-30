#pragma once

#include "process_maps.hpp"

#include <elf.h>
#include <link.h>
#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace yuki::solist::layout {

struct Memory {
  const std::vector<yukizygisk::MapInfo> &maps;

  [[nodiscard]] bool covers(uintptr_t address, size_t size,
                            bool readable = true) const {
    if (address == 0 || size == 0 || size > UINTPTR_MAX - address)
      return false;
    const uintptr_t end = address + size;
    for (const auto &map : maps) {
      if (map.end <= address)
        continue;
      if (map.start > address || (readable && !(map.perms & PROT_READ)))
        return false;
      if (map.end >= end)
        return true;
      address = map.end;
    }
    return false;
  }

  template <typename T> bool read(uintptr_t address, T &value) const {
    if (!covers(address, sizeof(T)))
      return false;
    memcpy(&value, reinterpret_cast<const void *>(address), sizeof(T));
    return true;
  }

  template <typename T>
  bool field(uintptr_t object, size_t offset, T &value) const {
    return offset <= UINTPTR_MAX - object && read(object + offset, value);
  }

  [[nodiscard]] bool text(uintptr_t address, std::string_view expected) const {
    return expected.size() < SIZE_MAX && covers(address, expected.size() + 1) &&
           memcmp(reinterpret_cast<const void *>(address), expected.data(),
                  expected.size()) == 0 &&
           *reinterpret_cast<const char *>(address + expected.size()) == '\0';
  }
};

struct Image {
  uintptr_t bias = 0;
  uintptr_t base = 0;
  size_t size = 0;
  uintptr_t dynamic = 0;
  uintptr_t phdr = 0;
  size_t phnum = 0;
};

inline bool add(uintptr_t base, uint64_t offset, uintptr_t &result) {
  if (offset > UINTPTR_MAX - base)
    return false;
  result = base + static_cast<uintptr_t>(offset);
  return true;
}

inline bool describe(const Memory &memory, uintptr_t phdr, size_t count,
                     uintptr_t bias, size_t page, Image &out) {
  if (count == 0 || count > 128 || page == 0 || (page & (page - 1)) != 0 ||
      phdr % alignof(ElfW(Phdr)) != 0 ||
      !memory.covers(phdr, count * sizeof(ElfW(Phdr))))
    return false;
  uintptr_t low = UINTPTR_MAX, high = 0, dynamic = 0, dynamic_end = 0;
  bool headers = false;
  for (size_t i = 0; i < count; ++i) {
    ElfW(Phdr) ph{};
    if (!memory.field(phdr, i * sizeof(ph), ph))
      return false;
    if (ph.p_type != PT_LOAD && ph.p_type != PT_DYNAMIC)
      continue;
    uintptr_t start = 0, end = 0;
    if (ph.p_filesz > ph.p_memsz || !add(bias, ph.p_vaddr, start) ||
        !add(start, ph.p_memsz, end))
      return false;
    if (ph.p_type == PT_DYNAMIC) {
      if (dynamic != 0 || ph.p_memsz < sizeof(ElfW(Dyn)) ||
          !memory.covers(start, sizeof(ElfW(Dyn))))
        return false;
      dynamic = start;
      dynamic_end = end;
      continue;
    }
    if (ph.p_align > 1 && ((ph.p_align & (ph.p_align - 1)) != 0 ||
                           ph.p_offset % ph.p_align != ph.p_vaddr % ph.p_align))
      return false;
    if (ph.p_memsz == 0)
      continue;
    if (!memory.covers(start, ph.p_memsz, false) ||
        end > UINTPTR_MAX - (page - 1))
      return false;
    if (start <= phdr && phdr < end && count * sizeof(ElfW(Phdr)) <= end - phdr)
      headers = true;
    low = std::min(low, start & ~(static_cast<uintptr_t>(page) - 1));
    high =
        std::max(high, (end + page - 1) & ~(static_cast<uintptr_t>(page) - 1));
  }
  if (!headers || high <= low || dynamic < low || dynamic >= high)
    return false;
  bool dynamic_in_load = false;
  for (size_t i = 0; i < count; ++i) {
    ElfW(Phdr) ph{};
    uintptr_t start = 0, end = 0;
    if (!memory.field(phdr, i * sizeof(ph), ph))
      return false;
    if (ph.p_type == PT_LOAD && add(bias, ph.p_vaddr, start) &&
        add(start, ph.p_memsz, end) && start <= dynamic && dynamic_end <= end)
      dynamic_in_load = true;
  }
  if (!dynamic_in_load)
    return false;
  out = {bias, low, high - low, dynamic, phdr, count};
  return true;
}

inline bool main_image(const Memory &memory, uintptr_t phdr, size_t count,
                       size_t page, Image &out) {
  if (count == 0 || count > 128 ||
      !memory.covers(phdr, count * sizeof(ElfW(Phdr))))
    return false;
  uintptr_t bias = 0;
  size_t matches = 0;
  for (size_t i = 0; i < count; ++i) {
    ElfW(Phdr) ph{};
    if (!memory.field(phdr, i * sizeof(ph), ph))
      return false;
    if (ph.p_type == PT_PHDR) {
      if (++matches != 1 || ph.p_vaddr > phdr ||
          ph.p_filesz < count * sizeof(ph))
        return false;
      bias = phdr - ph.p_vaddr;
    }
  }
  return matches == 1 && describe(memory, phdr, count, bias, page, out);
}

inline bool linker_image(const Memory &memory, uintptr_t header, size_t page,
                         Image &out) {
  ElfW(Ehdr) eh{};
  uintptr_t phdr = 0;
  if (!memory.read(header, eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB ||
      eh.e_ident[EI_VERSION] != EV_CURRENT || eh.e_version != EV_CURRENT ||
      eh.e_type != ET_DYN || eh.e_ehsize != sizeof(eh) ||
      eh.e_phentsize != sizeof(ElfW(Phdr)) || eh.e_phnum == 0 ||
      eh.e_phnum > 128 || !add(header, eh.e_phoff, phdr) ||
      !memory.covers(phdr, eh.e_phnum * sizeof(ElfW(Phdr))))
    return false;
  uintptr_t bias = 0;
  size_t matches = 0;
  for (size_t i = 0; i < eh.e_phnum; ++i) {
    ElfW(Phdr) ph{};
    if (!memory.field(phdr, i * sizeof(ph), ph))
      return false;
    if (ph.p_type == PT_LOAD && ph.p_offset == 0 && ph.p_filesz >= sizeof(eh)) {
      if (++matches != 1 || ph.p_vaddr > header)
        return false;
      bias = header - ph.p_vaddr;
    }
  }
  return matches == 1 && describe(memory, phdr, eh.e_phnum, bias, page, out);
}

struct Offsets {
  size_t base = 0;
  size_t size = 0;
  size_t next = 0;
  size_t ctor = 0;
};

enum class Result { Valid, ImageMismatch, LinkMapMismatch, ListMismatch };

inline bool image_fields(const Memory &memory, uintptr_t object, size_t offset,
                         const Image &image, bool synthetic_linker = false) {
  uintptr_t base = 0;
  size_t size = 0;
  return memory.field(object, offset, base) &&
         memory.field(object, offset + sizeof(void *), size) &&
         ((base == image.base && size == image.size) ||
          (synthetic_linker && base == 0 && size == 0));
}

inline bool header_fields(const Memory &memory, uintptr_t object, size_t offset,
                          const Image &image) {
  uintptr_t phdr = 0;
  size_t count = 0;
  return memory.field(object, offset, phdr) &&
         memory.field(object, offset + sizeof(void *), count) &&
         phdr == image.phdr && count == image.phnum;
}

inline bool link_map_fields(const Memory &memory, uintptr_t object,
                            size_t offset, const Image &image, link_map &lm) {
  unsigned char called = 0;
  return memory.field(object, offset, lm) && lm.l_addr == image.bias &&
         reinterpret_cast<uintptr_t>(lm.l_ld) == image.dynamic &&
         memory.field(object, offset + sizeof(lm), called) && called <= 1;
}

inline Result probe(const Memory &memory, uintptr_t main, uintptr_t linker,
                    uintptr_t vdso, const Image &main_image,
                    const Image &linker_image, std::string_view linker_path,
                    Offsets &out) {
  constexpr size_t kScanBytes = 1024;
  Offsets candidate;
  size_t matches = 0;
  for (size_t off = 0; off + 2 * sizeof(void *) <= kScanBytes;
       off += sizeof(void *)) {
    if (header_fields(memory, main, off, main_image) &&
        header_fields(memory, linker, off, linker_image))
      ++matches;
  }
  if (matches != 1)
    return Result::ImageMismatch;
  matches = 0;
  for (size_t off = 0; off + 2 * sizeof(void *) <= kScanBytes;
       off += sizeof(void *)) {
    // Older Bionic leaves base/size unset in its synthetic linker soinfo.
    if (image_fields(memory, main, off, main_image) &&
        image_fields(memory, linker, off, linker_image, true)) {
      ++matches;
      candidate.base = off;
      candidate.size = off + sizeof(void *);
    }
  }
  if (matches != 1)
    return Result::ImageMismatch;

  matches = 0;
  for (size_t off = 0; off + sizeof(link_map) + 1 <= kScanBytes;
       off += sizeof(void *)) {
    link_map lm{}, mm{};
    unsigned char called = 0;
    if (link_map_fields(memory, linker, off, linker_image, lm) &&
        link_map_fields(memory, main, off, main_image, mm) &&
        memory.text(reinterpret_cast<uintptr_t>(lm.l_name), linker_path) &&
        memory.field(linker, off + sizeof(lm), called) && called == 1) {
      ++matches;
      candidate.ctor = off + sizeof(lm);
    }
  }
  if (matches != 1)
    return Result::LinkMapMismatch;

  matches = 0;
  for (size_t off = 0; off + sizeof(void *) <= kScanBytes;
       off += sizeof(void *)) {
    uintptr_t next = 0;
    // solist and the debugger's link_map chain need not have the same order.
    if (memory.field(linker, off, next) && next != 0 &&
        (next == main || (vdso != 0 && next == vdso)) &&
        next % alignof(void *) == 0 && memory.covers(next, sizeof(void *))) {
      ++matches;
      candidate.next = off;
    }
  }
  if (matches != 1)
    return Result::ListMismatch;
  out = candidate;
  return Result::Valid;
}

} // namespace yuki::solist::layout
