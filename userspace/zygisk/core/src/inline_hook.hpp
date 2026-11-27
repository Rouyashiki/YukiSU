#pragma once

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#include "ihook_a64.hpp"
#include "ihook_arm32.hpp"
#include "ihook_lifecycle.hpp"
#include "ihook_pool.hpp"
#include "ihook_quiesce.hpp"
#include "ihook_sync.hpp"

namespace yuki::ihook {

struct InstallOptions {
  bool follow_thunk = false;
  std::atomic<uintptr_t> *pre_publish_trampoline_slot = nullptr;
  std::atomic<uintptr_t> *pre_publish_entry_slot = nullptr;
};

extern "C" uint8_t yz_patch_text_checked(uintptr_t addr, const void *expected,
                                         const void *replacement,
                                         unsigned int len);

enum class PatchResult : uint8_t { Rejected, Applied, Indeterminate };

[[nodiscard]] inline PatchResult checked_patch_result(uintptr_t address,
                                                      const void *expected,
                                                      const void *replacement,
                                                      size_t size) {
  const uint8_t result = yz_patch_text_checked(address, expected, replacement,
                                               static_cast<unsigned int>(size));
  return result == 0   ? PatchResult::Rejected
         : result == 1 ? PatchResult::Applied
                       : PatchResult::Indeterminate;
}

inline bool within_page(uintptr_t address, size_t size) {
  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  return size != 0 && address <= UINTPTR_MAX - size &&
         address % page_size + size <= page_size;
}

[[nodiscard]] inline bool page_range_executable(uintptr_t address,
                                                size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size)
    return false;
  if (within_page(address, size))
    return true;
  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  const uintptr_t first_page = address - (address % page_size);
  const uintptr_t last_byte = address + size - 1U;
  const uintptr_t last_page = last_byte - (last_byte % page_size);
  if (last_page != first_page + page_size)
    return false;
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return false;
  uintptr_t cov_begin = 0;
  uintptr_t cov_end = 0;
  bool have = false;
  char line[512];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    if (strchr(line, '\n') == nullptr) {
      int ch = 0;
      while ((ch = fgetc(maps)) != EOF && ch != '\n') {
      }
    }
    unsigned long long start = 0;
    unsigned long long end = 0;
    char perms[5] = {};
    if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3 ||
        start >= end || end > UINTPTR_MAX)
      continue;
    if (perms[0] != 'r' || perms[2] != 'x')
      continue;
    const uintptr_t b = static_cast<uintptr_t>(start);
    const uintptr_t e = static_cast<uintptr_t>(end);
    if (!have) {
      if (address >= b && address < e) {
        cov_begin = b;
        cov_end = e;
        have = true;
      }
    } else if (b == cov_end) {
      cov_end = e;
    }
  }
  (void)fclose(maps);
  return have && cov_begin <= address && cov_end >= address + size;
}

inline bool hook_ranges_overlap(uintptr_t first, size_t first_size,
                                uintptr_t second, size_t second_size) {
  if (first_size == 0 || second_size == 0 || first > UINTPTR_MAX - first_size ||
      second > UINTPTR_MAX - second_size)
    return true;
  return first < second + second_size && second < first + first_size;
}

inline bool single_threaded_process() {
  DIR *tasks = opendir("/proc/self/task");
  if (tasks == nullptr)
    return false;
  unsigned int count = 0;
  while (const dirent *entry = readdir(tasks)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9' && ++count > 1)
      break;
  }
  closedir(tasks);
  return count == 1;
}

[[nodiscard]] inline PatchResult
patch_bytes_checked(uintptr_t addr, const void *expected,
                    const void *replacement, size_t len,
                    bool single_word_atomic, uintptr_t guard_start,
                    uintptr_t guard_end, bool *concurrent_rejected) {
  if (concurrent_rejected != nullptr)
    *concurrent_rejected = false;
  if (len == 0 || expected == nullptr || replacement == nullptr)
    return PatchResult::Rejected;
  const bool single_page = within_page(addr, len);
  const bool single_thread = single_threaded_process();
  QuiesceGuard quiesce;
  if (!single_thread) {
    if (!quiesce.acquire(guard_start, guard_end)) {
      if (!single_page || !single_word_atomic) {
        if (concurrent_rejected != nullptr)
          *concurrent_rejected = true;
        return PatchResult::Rejected;
      }
    }
  }
  if (single_page)
    return checked_patch_result(addr, expected, replacement, len);

  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  const uintptr_t boundary = addr - (addr % page_size) + page_size;
  if (boundary <= addr || boundary - addr >= len ||
      !page_range_executable(addr, len))
    return PatchResult::Rejected;
  const size_t first_len = static_cast<size_t>(boundary - addr);
  const size_t second_len = len - first_len;
  const auto *exp_bytes = static_cast<const uint8_t *>(expected);
  const auto *rep_bytes = static_cast<const uint8_t *>(replacement);

  const PatchResult second = checked_patch_result(
      boundary, exp_bytes + first_len, rep_bytes + first_len, second_len);
  if (second != PatchResult::Applied)
    return second;

  const PatchResult first =
      checked_patch_result(addr, exp_bytes, rep_bytes, first_len);
  if (first == PatchResult::Applied)
    return PatchResult::Applied;
  if (first == PatchResult::Indeterminate)
    return PatchResult::Indeterminate;

  const PatchResult rollback = checked_patch_result(
      boundary, rep_bytes + first_len, exp_bytes + first_len, second_len);
  return rollback == PatchResult::Applied ? PatchResult::Rejected
                                          : PatchResult::Indeterminate;
}

enum class UnhookMode : uint8_t {
  RestoreBytes,
  DiscardCowPages,
};

struct TargetBounds {
  size_t vma_remaining = 0;
  size_t symbol_size = 0;
  bool have_vma = false;
  bool have_symbol_size = false;
};

namespace bounds_detail {

struct VmaRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
};

[[nodiscard]] inline bool ptr_in_vmas(const VmaRange *vmas, size_t count,
                                      uintptr_t addr, size_t size) {
  if (size == 0 || addr > UINTPTR_MAX - size)
    return false;
  const uintptr_t end = addr + size;
  for (size_t i = 0; i != count; ++i) {
    if (addr >= vmas[i].begin && end <= vmas[i].end)
      return true;
  }
  return false;
}

[[nodiscard]] inline size_t vma_tail_from(const VmaRange *vmas, size_t count,
                                          uintptr_t addr) {
  for (size_t i = 0; i != count; ++i) {
    if (addr >= vmas[i].begin && addr < vmas[i].end)
      return static_cast<size_t>(vmas[i].end - addr);
  }
  return 0;
}

[[nodiscard]] inline size_t gnu_hash_sym_count(const VmaRange *vmas,
                                               size_t vma_count,
                                               uintptr_t gnu_hash_addr) {
  if (!ptr_in_vmas(vmas, vma_count, gnu_hash_addr, 16U))
    return 0;
  const auto *hdr = reinterpret_cast<const uint32_t *>(gnu_hash_addr);
  const uint32_t nbuckets = hdr[0];
  const uint32_t symoffset = hdr[1];
  const uint32_t bloom_size = hdr[2];
  if (nbuckets == 0 || bloom_size == 0 || nbuckets > 65536U ||
      bloom_size > 65536U)
    return 0;
  const size_t bloom_bytes =
      static_cast<size_t>(bloom_size) * sizeof(uintptr_t);
  const uintptr_t buckets_addr = gnu_hash_addr + 16U + bloom_bytes;
  const size_t buckets_bytes = static_cast<size_t>(nbuckets) * sizeof(uint32_t);
  if (buckets_addr < gnu_hash_addr ||
      !ptr_in_vmas(vmas, vma_count, buckets_addr, buckets_bytes))
    return 0;
  const auto *buckets = reinterpret_cast<const uint32_t *>(buckets_addr);
  uint32_t max_sym = 0;
  for (uint32_t i = 0; i != nbuckets; ++i) {
    max_sym = std::max(buckets[i], max_sym);
  }
  if (max_sym < symoffset)
    return symoffset;
  const uintptr_t chains_addr = buckets_addr + buckets_bytes;
  size_t idx = static_cast<size_t>(max_sym - symoffset);
  for (size_t step = 0; step != 65536U; ++step) {
    const uintptr_t entry_addr = chains_addr + (idx * sizeof(uint32_t));
    if (!ptr_in_vmas(vmas, vma_count, entry_addr, sizeof(uint32_t)))
      return 0;
    const uint32_t chain_val = *reinterpret_cast<const uint32_t *>(entry_addr);
    if ((chain_val & 1U) != 0U)
      return static_cast<size_t>(symoffset) + idx + 1U;
    ++idx;
  }
  return 0;
}

[[nodiscard]] inline bool exec_addr_readable(uintptr_t address,
                                             size_t size = 4U) {
  if (size == 0 || address > UINTPTR_MAX - size)
    return false;
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return false;
  char line[512];
  bool ok = false;
  while (fgets(line, sizeof(line), maps) != nullptr) {
    if (strchr(line, '\n') == nullptr) {
      int ch = 0;
      while ((ch = fgetc(maps)) != EOF && ch != '\n') {
      }
    }
    unsigned long long start = 0;
    unsigned long long end = 0;
    char perms[5] = {};
    if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 &&
        start < end && address >= start && address + size <= end &&
        perms[0] == 'r' && perms[2] == 'x') {
      ok = true;
      break;
    }
  }
  (void)fclose(maps);
  return ok;
}

inline void scan_file_symtab(const char *path, uintptr_t elf_base,
                             uintptr_t target_vma_begin,
                             unsigned long long target_vma_off,
                             uintptr_t target_addr, size_t *exact_size,
                             size_t *next_gap) {
  if (path == nullptr || path[0] != '/' || exact_size == nullptr ||
      next_gap == nullptr)
    return;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return;
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      static_cast<uint64_t>(st.st_size) > uint64_t{128} * 1024U * 1024U) {
    close(fd);
    return;
  }
  const size_t file_size = static_cast<size_t>(st.st_size);
  void *mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mapped == MAP_FAILED)
    return;
  const auto *base = static_cast<const uint8_t *>(mapped);
  constexpr uint8_t kWantClass = sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
  if (file_size >= sizeof(ElfW(Ehdr))) {
    const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
        eh->e_ident[EI_CLASS] == kWantClass && eh->e_phoff != 0 &&
        eh->e_phentsize == sizeof(ElfW(Phdr)) && eh->e_phnum > 0 &&
        eh->e_phnum <= 64U &&
        static_cast<uint64_t>(eh->e_phoff) +
                static_cast<uint64_t>(eh->e_phnum) * sizeof(ElfW(Phdr)) <=
            file_size) {
      const auto *phdrs =
          reinterpret_cast<const ElfW(Phdr) *>(base + eh->e_phoff);
      uintptr_t min_vaddr = UINTPTR_MAX;
      uintptr_t bias = 0;
      bool have_bias = false;
      const uint64_t target_file_off =
          static_cast<uint64_t>(target_vma_off) +
          static_cast<uint64_t>(target_addr - target_vma_begin);
      for (unsigned i = 0; i != eh->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD)
          continue;
        if (phdrs[i].p_vaddr < min_vaddr)
          min_vaddr = static_cast<uintptr_t>(phdrs[i].p_vaddr);
        if (target_file_off >= phdrs[i].p_offset &&
            target_file_off < phdrs[i].p_offset + phdrs[i].p_filesz) {
          bias = target_addr -
                 static_cast<uintptr_t>(phdrs[i].p_vaddr +
                                        (target_file_off - phdrs[i].p_offset));
          have_bias = true;
        }
      }
      if (!have_bias && elf_base != 0 && min_vaddr != UINTPTR_MAX) {
        bias = elf_base - min_vaddr;
        have_bias = true;
      }
      if (have_bias && eh->e_shoff != 0 &&
          eh->e_shentsize == sizeof(ElfW(Shdr)) && eh->e_shnum > 0 &&
          static_cast<uint64_t>(eh->e_shoff) +
                  static_cast<uint64_t>(eh->e_shnum) * sizeof(ElfW(Shdr)) <=
              file_size) {
        const auto *shdrs =
            reinterpret_cast<const ElfW(Shdr) *>(base + eh->e_shoff);
        for (unsigned s = 0; s != eh->e_shnum; ++s) {
          if (shdrs[s].sh_type != SHT_SYMTAB ||
              shdrs[s].sh_entsize != sizeof(ElfW(Sym)) ||
              shdrs[s].sh_offset > file_size ||
              shdrs[s].sh_size > file_size - shdrs[s].sh_offset)
            continue;
          const auto *syms =
              reinterpret_cast<const ElfW(Sym) *>(base + shdrs[s].sh_offset);
          const size_t sym_count =
              static_cast<size_t>(shdrs[s].sh_size / sizeof(ElfW(Sym)));
          for (size_t i = 0; i != sym_count; ++i) {
            const ElfW(Sym) &sym = syms[i];
            if (ELF32_ST_TYPE(sym.st_info) != STT_FUNC ||
                sym.st_shndx == SHN_UNDEF || sym.st_value == 0)
              continue;
            const uintptr_t addr =
                (bias + static_cast<uintptr_t>(sym.st_value)) & ~uintptr_t{1};
            if (addr == target_addr) {
              if (sym.st_size != 0) {
                const size_t sz = static_cast<size_t>(sym.st_size);
                *exact_size = *exact_size == 0 ? sz : std::min(sz, *exact_size);
              }
            } else if (addr > target_addr) {
              const size_t gap = static_cast<size_t>(addr - target_addr);
              *next_gap = std::min(gap, *next_gap);
            }
          }
        }
      }
    }
  }
  munmap(mapped, file_size);
}

} // namespace bounds_detail

inline void query_target_bounds(uintptr_t target_addr, TargetBounds *out) {
  if (out == nullptr)
    return;
  *out = TargetBounds{};
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return;

  struct MapRecord {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    unsigned long long offset = 0;
    unsigned dev_major = 0;
    unsigned dev_minor = 0;
    unsigned long long inode = 0;
    bool readable = false;
    bool valid = false;
  };
  MapRecord target_vma{};
  char target_path[512]{};

  char line[1024];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    if (strchr(line, '\n') == nullptr) {
      int ch = 0;
      while ((ch = fgetc(maps)) != EOF && ch != '\n') {
      }
    }
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long offset = 0;
    unsigned maj = 0;
    unsigned min = 0;
    unsigned long long inode = 0;
    char perms[5] = {};
    int path_off = 0;
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &start, &end, perms,
               &offset, &maj, &min, &inode, &path_off) != 7 ||
        start >= end || end > UINTPTR_MAX)
      continue;
    if (target_addr >= start && target_addr < end) {
      target_vma = {static_cast<uintptr_t>(start),
                    static_cast<uintptr_t>(end),
                    offset,
                    maj,
                    min,
                    inode,
                    perms[0] == 'r',
                    true};
      if (path_off > 0) {
        const char *raw_path = line + path_off;
        size_t plen = strcspn(raw_path, "\r\n");
        if (plen > 10 && memcmp(raw_path + plen - 10, " (deleted)", 10) == 0)
          plen -= 10;
        if (plen < sizeof(target_path)) {
          memcpy(target_path, raw_path, plen);
          target_path[plen] = '\0';
        }
      }
      break;
    }
  }

  if (!target_vma.valid || !target_vma.readable) {
    (void)fclose(maps);
    return;
  }
  out->have_vma = true;
  out->vma_remaining = static_cast<size_t>(target_vma.end - target_addr);
  size_t exact_size = 0;
  size_t next_gap = SIZE_MAX;
  uintptr_t elf_base = 0;

  constexpr size_t kMaxObjVmas = 32;
  bounds_detail::VmaRange vmas[kMaxObjVmas]{};
  size_t vma_count = 0;
  if (target_vma.inode != 0) {
    if (fseek(maps, 0, SEEK_SET) != 0) {
      (void)fclose(maps);
      return;
    }
    while (fgets(line, sizeof(line), maps) != nullptr) {
      if (strchr(line, '\n') == nullptr) {
        int ch = 0;
        while ((ch = fgetc(maps)) != EOF && ch != '\n') {
        }
      }
      unsigned long long start = 0;
      unsigned long long end = 0;
      unsigned long long offset = 0;
      unsigned maj = 0;
      unsigned min = 0;
      unsigned long long inode = 0;
      char perms[5] = {};
      if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu", &start, &end, perms,
                 &offset, &maj, &min, &inode) != 7 ||
          start >= end || end > UINTPTR_MAX || perms[0] != 'r')
        continue;
      if (inode != target_vma.inode || maj != target_vma.dev_major ||
          min != target_vma.dev_minor)
        continue;
      const uintptr_t b = static_cast<uintptr_t>(start);
      const uintptr_t e = static_cast<uintptr_t>(end);
      if (offset == 0 && elf_base == 0)
        elf_base = b;
      if (vma_count < kMaxObjVmas)
        vmas[vma_count++] = {b, e};
    }
  }
  (void)fclose(maps);

  if (target_vma.inode != 0) {
    if (elf_base != 0 && bounds_detail::ptr_in_vmas(vmas, vma_count, elf_base,
                                                    sizeof(ElfW(Ehdr)))) {
      const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(elf_base);
      constexpr uint8_t kWantClass =
          sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
      if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
          eh->e_ident[EI_CLASS] == kWantClass &&
          eh->e_phentsize == sizeof(ElfW(Phdr)) && eh->e_phnum != 0 &&
          eh->e_phnum <= 64U) {
        const uintptr_t phdr_addr =
            elf_base + static_cast<uintptr_t>(eh->e_phoff);
        const size_t phdr_bytes =
            static_cast<size_t>(eh->e_phnum) * sizeof(ElfW(Phdr));
        if (bounds_detail::ptr_in_vmas(vmas, vma_count, phdr_addr,
                                       phdr_bytes)) {
          const auto *phdrs = reinterpret_cast<const ElfW(Phdr) *>(phdr_addr);
          uintptr_t min_vaddr = UINTPTR_MAX;
          const ElfW(Phdr) *dyn_phdr = nullptr;
          for (unsigned i = 0; i != eh->e_phnum; ++i) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr < min_vaddr)
              min_vaddr = static_cast<uintptr_t>(phdrs[i].p_vaddr);
            else if (phdrs[i].p_type == PT_DYNAMIC)
              dyn_phdr = &phdrs[i];
          }
          if (min_vaddr != UINTPTR_MAX && dyn_phdr != nullptr &&
              dyn_phdr->p_memsz != 0) {
            const uintptr_t load_bias = elf_base - min_vaddr;
            const uintptr_t dyn_addr =
                load_bias + static_cast<uintptr_t>(dyn_phdr->p_vaddr);
            const size_t dyn_bytes = static_cast<size_t>(dyn_phdr->p_memsz);
            if (bounds_detail::ptr_in_vmas(vmas, vma_count, dyn_addr,
                                           dyn_bytes)) {
              uintptr_t symtab_addr = 0;
              uintptr_t strtab_addr = 0;
              uintptr_t hash_addr = 0;
              uintptr_t gnu_hash_addr = 0;
              size_t syment = sizeof(ElfW(Sym));
              const auto *dyn = reinterpret_cast<const ElfW(Dyn) *>(dyn_addr);
              const size_t dyn_count = dyn_bytes / sizeof(ElfW(Dyn));
              for (size_t i = 0; i != dyn_count && dyn[i].d_tag != DT_NULL;
                   ++i) {
                const uintptr_t val = static_cast<uintptr_t>(dyn[i].d_un.d_ptr);
                switch (dyn[i].d_tag) {
                case DT_SYMTAB:
                  symtab_addr = val < elf_base ? load_bias + val : val;
                  break;
                case DT_STRTAB:
                  strtab_addr = val < elf_base ? load_bias + val : val;
                  break;
                case DT_SYMENT:
                  syment = static_cast<size_t>(dyn[i].d_un.d_val);
                  break;
                case DT_HASH:
                  hash_addr = val < elf_base ? load_bias + val : val;
                  break;
                case DT_GNU_HASH:
                  gnu_hash_addr = val < elf_base ? load_bias + val : val;
                  break;
                default:
                  break;
                }
              }
              if (symtab_addr != 0 && syment == sizeof(ElfW(Sym))) {
                size_t sym_count = 0;
                if (hash_addr != 0 && bounds_detail::ptr_in_vmas(
                                          vmas, vma_count, hash_addr, 8U)) {
                  sym_count = reinterpret_cast<const uint32_t *>(hash_addr)[1];
                } else if (gnu_hash_addr != 0) {
                  sym_count = bounds_detail::gnu_hash_sym_count(vmas, vma_count,
                                                                gnu_hash_addr);
                } else if (strtab_addr > symtab_addr) {
                  sym_count =
                      static_cast<size_t>(strtab_addr - symtab_addr) / syment;
                }
                const size_t vma_sym_tail =
                    bounds_detail::vma_tail_from(vmas, vma_count, symtab_addr);
                if (sym_count != 0 && vma_sym_tail >= syment) {
                  sym_count = std::min(sym_count, vma_sym_tail / syment);
                  const auto *syms =
                      reinterpret_cast<const ElfW(Sym) *>(symtab_addr);
                  for (size_t i = 0; i != sym_count; ++i) {
                    const ElfW(Sym) &sym = syms[i];
                    if (ELF32_ST_TYPE(sym.st_info) != STT_FUNC ||
                        sym.st_shndx == SHN_UNDEF || sym.st_value == 0)
                      continue;
                    const uintptr_t addr =
                        (load_bias + static_cast<uintptr_t>(sym.st_value)) &
                        ~uintptr_t{1};
                    if (addr == target_addr) {
                      if (sym.st_size != 0) {
                        const size_t sz = static_cast<size_t>(sym.st_size);
                        exact_size =
                            exact_size == 0 ? sz : std::min(sz, exact_size);
                      }
                    } else if (addr > target_addr) {
                      const size_t gap =
                          static_cast<size_t>(addr - target_addr);
                      next_gap = std::min(gap, next_gap);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  if (exact_size == 0 && target_path[0] == '/') {
    bounds_detail::scan_file_symtab(target_path, elf_base, target_vma.begin,
                                    target_vma.offset, target_addr, &exact_size,
                                    &next_gap);
  }

  size_t bound = out->vma_remaining;
  if (exact_size != 0) {
    bound = std::min(exact_size, bound);
    bound = std::min(next_gap, bound);
    out->symbol_size = bound;
    out->have_symbol_size = true;
  } else if (next_gap != SIZE_MAX) {
    out->symbol_size = std::min(next_gap, bound);
    out->have_symbol_size = true;
  }
}

inline bool discard_cow_patch_pages(uintptr_t address, const void *patched,
                                    const void *saved, size_t patch_size,
                                    HookPhase *phase = nullptr) {
  if (!single_threaded_process() || !within_page(address, patch_size) ||
      memcmp(reinterpret_cast<const void *>(address), patched, patch_size) != 0)
    return false;
  const size_t page_size = static_cast<size_t>(getpagesize());
  const uintptr_t first_page = address - (address % page_size);
  FILE *maps = fopen("/proc/self/maps", "re");
  if (maps == nullptr)
    return false;
  char line[8192];
  char path[4096];
  unsigned long start = 0;
  unsigned long end = 0;
  unsigned long long offset = 0;
  char perms[5] = {};
  bool found = false;
  while (fgets(line, sizeof(line), maps) != nullptr) {
    path[0] = '\0';
    if (sscanf(line, "%lx-%lx %4s %llx %*s %*s  %4095[^\n]", &start, &end,
               perms, &offset, path) != 5)
      continue;
    if (first_page >= start && first_page < end &&
        end - first_page >= page_size && perms[3] == 'p' && path[0] == '/') {
      found = true;
      break;
    }
  }
  (void)fclose(maps);
  if (!found)
    return false;

  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  auto *image = static_cast<uint8_t *>(malloc(page_size * 2));
  if (image == nullptr) {
    close(fd);
    return false;
  }
  auto *backing = image + page_size;
  memcpy(image, reinterpret_cast<const void *>(first_page), page_size);
  memcpy(image + address - first_page, saved, patch_size);
  const off_t file_offset = static_cast<off_t>(offset + first_page - start);
  const bool unchanged = pread(fd, backing, page_size, file_offset) ==
                             static_cast<ssize_t>(page_size) &&
                         memcmp(image, backing, page_size) == 0;
  free(image);
  close(fd);
  if (!unchanged)
    return false;
  if (madvise(reinterpret_cast<void *>(first_page), page_size, MADV_DONTNEED) !=
      0)
    return false;
  if (memcmp(reinterpret_cast<const void *>(address), saved, patch_size) != 0) {
    fail_indeterminate(phase);
  }
  return true;
}

} // namespace yuki::ihook

#if defined(__aarch64__)
extern "C" {
extern uint8_t yz_cap_tmpl[];
extern uint8_t yz_cap_tmpl_ctx[];
extern uint8_t yz_cap_tmpl_wrap[];
extern uint8_t yz_cap_tmpl_end[];
extern uint64_t g_yz_ret_ctx[];
/* COW patch through zygiskd/kernel. */
bool yz_patch_text(uintptr_t addr, const void *bytes, unsigned int len);
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr;
  uint32_t saved[a64::kMaxStolen] = {};
  uint32_t patch[a64::kMaxStolen] = {};
  void *trampoline = nullptr;
  void *islands[a64::kMaxStolen + 1U] = {};
  bool trampoline_pooled = false;
  bool island_pooled[a64::kMaxStolen + 1U] = {};
  uint8_t island_count = 0;
  uint8_t patched_size = 0;
  bool active = false;
  bool restored = false;
  HookPhase phase = HookPhase::Released;
  InstructionSyncPlan sync_plan{};
};

[[nodiscard]] inline int trampoline_protection() {
  return PROT_READ | PROT_EXEC |
         ((getauxval(AT_HWCAP2) & HWCAP2_BTI) != 0 ? PROT_BTI : 0);
}

[[nodiscard]] inline bool target_page_is_bti_guarded(uintptr_t target) {
  if ((getauxval(AT_HWCAP2) & HWCAP2_BTI) == 0)
    return false;
  FILE *smaps = fopen("/proc/self/smaps", "re");
  if (smaps == nullptr)
    return true; // Conservative when smaps is unreadable.
  char line[1024];
  bool in_vma = false;
  bool guarded = true;
  bool saw_vmflags = false;
  while (fgets(line, sizeof(line), smaps) != nullptr) {
    if (strchr(line, '\n') == nullptr) {
      int ch = 0;
      while ((ch = fgetc(smaps)) != EOF && ch != '\n') {
      }
    }
    unsigned long long start = 0;
    unsigned long long end = 0;
    if (sscanf(line, "%llx-%llx ", &start, &end) == 2 && start < end) {
      if (in_vma)
        break;
      in_vma = target >= start && target < end;
      continue;
    }
    if (!in_vma)
      continue;
    if (strncmp(line, "VmFlags:", 8) == 0) {
      saw_vmflags = true;
      guarded = false;
      const char *p = line + 8;
      while (*p != '\0' && *p != '\n' && *p != '\r') {
        while (*p == ' ' || *p == '\t')
          ++p;
        if (*p == '\0' || *p == '\n' || *p == '\r')
          break;
        if (p[0] == 'b' && p[1] == 't' &&
            (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\r' ||
             p[2] == '\0')) {
          guarded = true;
          break;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '\r')
          ++p;
      }
      break;
    }
  }
  (void)fclose(smaps);
  return !in_vma || !saw_vmflags || guarded;
}

[[nodiscard]] inline uint32_t entry_landing(uint32_t instruction) {
  if (a64::is_bti_landing(instruction))
    return instruction;
  return 0xd50324dfU; // bti jc
}

namespace a64_detail {

[[nodiscard]] inline bool allocate_branch_island(uintptr_t target, bool is_call,
                                                 bool save_scratch, int prot,
                                                 bool allow_mprotect_reuse,
                                                 IslandSlot *slot_out) {
  constexpr uintptr_t reach = 0x7800000;
  const uintptr_t min_addr = target > reach ? target - reach : 4096U;
  const uintptr_t max_addr =
      target <= UINTPTR_MAX - reach ? target + reach : UINTPTR_MAX;
  const uintptr_t pc_off = (is_call || !save_scratch) ? 4U : 8U;
  IslandSlot slot = island_pool().alloc_near(
      min_addr, max_addr, target, prot,
      [&](uintptr_t hint) -> bool {
        return a64::branch(hint + pc_off, target) != 0;
      },
      allow_mprotect_reuse);
  if (!slot)
    return false;
  uint8_t buf[16]{};
  const size_t bytes = a64::emit_branch_island(
      buf, sizeof(buf), reinterpret_cast<uintptr_t>(slot.get()), target,
      is_call, save_scratch);
  if (bytes == 0 || !island_pool().commit_slot(slot, buf, bytes))
    return false;
  *slot_out = std::move(slot);
  return true;
}

[[nodiscard]] inline bool
populate_branch_islands(const a64::Plan &analyzed, uintptr_t tramp_co_base,
                        int prot, bool allow_mprotect_reuse,
                        IslandSlot *owned_slots, uint8_t *owned_count,
                        uint8_t max_slots, a64::RewriteIslands *islands) {
  for (size_t i = 0; i != analyzed.count; ++i) {
    const auto &insn = analyzed.instructions[i];
    if (!a64::is_direct_branch(insn.kind))
      continue;
    int64_t dummy = 0;
    // Allocate an island whenever the external branch could exceed 26-bit
    // B/BL reach anywhere inside the 512-byte trampoline slot.
    if (a64::immediate(tramp_co_base, insn.target, 26U, &dummy) &&
        a64::immediate(tramp_co_base + 384U, insn.target, 26U, &dummy))
      continue;
    if (*owned_count >= max_slots)
      return false;
    const bool is_call = insn.kind == a64::Kind::Call;
    const uint8_t scratch = a64::detail::select_far_scratch(analyzed, i);
    if (is_call && scratch == 31U)
      return false;
    const bool save_scratch = !is_call && scratch == 31U;
    if (!allocate_branch_island(insn.target, is_call, save_scratch, prot,
                                allow_mprotect_reuse,
                                &owned_slots[*owned_count]))
      return false;
    islands->branches[i] =
        reinterpret_cast<uintptr_t>(owned_slots[*owned_count].get());
    islands->branch_save_scratch[i] = save_scratch;
    ++(*owned_count);
  }
  return true;
}

} // namespace a64_detail

/* Patch target prologue and return call-original trampoline. */
inline void *install(void *target, void *replacement, Hook *out,
                     bool prefer_relative = false, bool capture_return = false,
                     a64::Error *reason = nullptr, size_t max_patch_size = 20,
                     InstallOptions options = {}) {
  const auto reject = [reason](a64::Error error) -> void * {
    if (reason != nullptr)
      *reason = error;
    return nullptr;
  };
  if (reason != nullptr)
    *reason = a64::Error::None;
  uintptr_t target_addr = reinterpret_cast<uintptr_t>(target);
  if (target == nullptr || replacement == nullptr || out == nullptr ||
      out->active || out->phase != HookPhase::Released ||
      (target_addr & 3U) != 0 || !page_range_executable(target_addr, 4) ||
      max_patch_size < 4)
    return reject(a64::Error::InvalidInput);

  const size_t page_size = static_cast<size_t>(getpagesize());
  if (options.follow_thunk) {
    for (unsigned hop = 0; hop != 4U; ++hop) {
      const size_t avail = page_size - (target_addr % page_size);
      uintptr_t next = 0;
      if (!a64::resolve_thunk(reinterpret_cast<const uint8_t *>(target_addr),
                              avail, target_addr, &next))
        break;
      if (next == 0 || (next & 3U) != 0 || next == target_addr ||
          !bounds_detail::exec_addr_readable(next, 4U))
        break;
      target_addr = next;
    }
    target = reinterpret_cast<void *>(target_addr);
  }

  auto *t = reinterpret_cast<uint32_t *>(target);
  const auto *target_bytes = reinterpret_cast<const uint8_t *>(target);

  const auto cap_base = reinterpret_cast<uintptr_t>(yz_cap_tmpl);
  const size_t cap_size =
      reinterpret_cast<uintptr_t>(yz_cap_tmpl_end) - cap_base;
  const size_t ctx_off =
      reinterpret_cast<uintptr_t>(yz_cap_tmpl_ctx) - cap_base;
  const size_t wrap_off =
      reinterpret_cast<uintptr_t>(yz_cap_tmpl_wrap) - cap_base;
  const size_t co_off = (cap_size + 3U) & ~static_cast<size_t>(3); // 4-aligned
  if (co_off > 512U - 240U || co_off > page_size - 240U)
    return reject(a64::Error::Capacity);

  uint32_t first_word = 0;
  memcpy(&first_word, target, sizeof(first_word));
  const bool need_landing = a64::needs_entry_landing(first_word);
  const size_t near_count = need_landing ? 2U : 1U;

  TargetBounds bounds{};
  query_target_bounds(target_addr, &bounds);
  size_t effective_max_patch = max_patch_size;
  if (bounds.have_vma && bounds.vma_remaining < effective_max_patch)
    effective_max_patch = bounds.vma_remaining;
  if (bounds.have_symbol_size && bounds.symbol_size < effective_max_patch)
    effective_max_patch = bounds.symbol_size;

  if (effective_max_patch < near_count * 4U)
    return reject(a64::Error::SymbolTooShort);
  if (!page_range_executable(target_addr, near_count * 4U))
    return reject(a64::Error::Capacity);

  const size_t scan_bytes =
      bounds.have_symbol_size
          ? bounds.symbol_size
          : (bounds.have_vma ? bounds.vma_remaining
                             : page_size - (target_addr % page_size));

  a64::Plan plan{};
  const a64::Error analyzed_near = a64::analyze(target_bytes, near_count * 4U,
                                                target_addr, near_count, &plan);
  if (analyzed_near != a64::Error::None)
    return reject(analyzed_near);
  if (a64::has_interior_branch(target_bytes, scan_bytes, target_addr,
                               near_count * 4U))
    return reject(a64::Error::InteriorBranch);

  const bool single_thread = single_threaded_process();
  const int tramp_prot = trampoline_protection();
  const bool bti_guarded = target_page_is_bti_guarded(target_addr);
  constexpr uintptr_t kReach = 0x7800000;
  const uintptr_t min_near =
      target_addr > kReach ? target_addr - kReach : page_size;
  const uintptr_t max_near =
      target_addr <= UINTPTR_MAX - kReach ? target_addr + kReach : UINTPTR_MAX;
  const uintptr_t entry_branch_pc = target_addr + (near_count == 1U ? 0U : 4U);

  TrampolineSlot tramp_slot;
  IslandSlot island_slots[a64::kMaxStolen + 1U];
  uint8_t island_count = 0;
  size_t covered_count = near_count;
  uintptr_t entry_branch_target = 0;
  bool use_far_entry = false;
  a64::Error last_error = a64::Error::Mapping;
  (void)prefer_relative;

  // Path A: Near pooled trampoline without islands.
  {
    const a64::EmitPolicy near_policy{false, true};
    tramp_slot = trampoline_pool().alloc_near(
        min_near, max_near, target_addr, tramp_prot,
        [&](uintptr_t hint) -> bool {
          if (a64::branch(entry_branch_pc, hint) == 0)
            return false;
          return a64::plan(target_bytes, near_count * 4U, target_addr,
                           hint + co_off + 4U, 512U - co_off - 4U, near_count,
                           near_policy, &plan) == a64::Error::None;
        },
        single_thread);
    if (tramp_slot)
      entry_branch_target = reinterpret_cast<uintptr_t>(tramp_slot.get());
  }

  // Path B: Near pooled trampoline + per-branch BTI-safe Rewrite Islands.
  if (!tramp_slot) {
    const uintptr_t resume_addr = target_addr + (near_count * 4U);
    TrampolineSlot cand = trampoline_pool().alloc_near(
        min_near, max_near, target_addr, tramp_prot,
        [&](uintptr_t hint) -> bool {
          return a64::branch(entry_branch_pc, hint) != 0 &&
                 (!plan.needs_continuation ||
                  a64::branch(hint + 480U, resume_addr) != 0);
        },
        single_thread);
    if (cand) {
      const uintptr_t tramp_base = reinterpret_cast<uintptr_t>(cand.get());
      a64::RewriteIslands islands{};
      IslandSlot branch_slots[a64::kMaxStolen + 1U];
      uint8_t branch_count = 0;
      // Filter external branches vs internal branches.
      a64::Plan check_plan = plan;
      for (size_t i = 0; i != check_plan.count; ++i) {
        auto &insn = check_plan.instructions[i];
        if (a64::is_direct_branch(insn.kind) && insn.target >= target_addr &&
            insn.target < resume_addr)
          insn.kind = a64::Kind::Copy;
      }
      if (a64_detail::populate_branch_islands(
              check_plan, tramp_base + co_off + 4U, tramp_prot, single_thread,
              branch_slots, &branch_count, a64::kMaxStolen, &islands) &&
          a64::plan(target_bytes, near_count * 4U, target_addr,
                    tramp_base + co_off + 4U, 512U - co_off - 4U, near_count,
                    a64::EmitPolicy{false, true}, islands,
                    &plan) == a64::Error::None) {
        tramp_slot = std::move(cand);
        for (uint8_t k = 0; k != branch_count; ++k)
          island_slots[k] = std::move(branch_slots[k]);
        island_count = branch_count;
        entry_branch_target = tramp_base;
      } else {
        last_error = plan.error != a64::Error::None ? plan.error
                                                    : a64::Error::Unreachable;
      }
    }
  }

  // Path C: Near 64B Entry+Resume Island + Far Pooled Trampoline (BTI/GCS
  // safe).
  if (!tramp_slot) {
    TrampolineSlot far_tramp = trampoline_pool().alloc_any(
        tramp_prot, [](uintptr_t) { return true; }, single_thread);
    if (far_tramp) {
      const uintptr_t tramp_base = reinterpret_cast<uintptr_t>(far_tramp.get());
      const uintptr_t resume_addr = target_addr + (near_count * 4U);
      a64::Plan check_plan = plan;
      for (size_t i = 0; i != check_plan.count; ++i) {
        auto &insn = check_plan.instructions[i];
        if (a64::is_direct_branch(insn.kind) && insn.target >= target_addr &&
            insn.target < resume_addr)
          insn.kind = a64::Kind::Copy;
      }
      const uint8_t cont_scratch =
          a64::detail::select_far_scratch(check_plan, check_plan.count);
      const bool resume_save = cont_scratch == 31U;
      IslandSlot entry_island = island_pool().alloc_near(
          min_near, max_near, target_addr, tramp_prot,
          [&](uintptr_t hint) -> bool {
            if (a64::branch(entry_branch_pc, hint) == 0)
              return false;
            return !plan.needs_continuation ||
                   a64::branch(hint + 24U + (resume_save ? 8U : 4U),
                               resume_addr) != 0;
          },
          single_thread);
      if (entry_island) {
        const uintptr_t entry_island_addr =
            reinterpret_cast<uintptr_t>(entry_island.get());
        uint8_t ibuf[48]{};
        size_t ibytes = a64::emit_entry_island(ibuf, sizeof(ibuf), tramp_base);
        uintptr_t resume_island_addr = 0;
        if (plan.needs_continuation) {
          const size_t rbytes = a64::emit_branch_island(
              ibuf + ibytes, sizeof(ibuf) - ibytes, entry_island_addr + ibytes,
              resume_addr, false, resume_save);
          if (rbytes != 0) {
            resume_island_addr = entry_island_addr + ibytes;
            ibytes += rbytes;
          } else {
            ibytes = 0;
          }
        }
        if (ibytes != 0 &&
            island_pool().commit_slot(entry_island, ibuf, ibytes)) {
          a64::RewriteIslands islands{};
          islands.resume = resume_island_addr;
          islands.resume_save_scratch = resume_save;
          IslandSlot branch_slots[a64::kMaxStolen + 1U];
          uint8_t branch_count = 0;
          if (a64_detail::populate_branch_islands(
                  check_plan, tramp_base + co_off + 4U, tramp_prot,
                  single_thread, branch_slots, &branch_count, a64::kMaxStolen,
                  &islands) &&
              a64::plan(target_bytes, near_count * 4U, target_addr,
                        tramp_base + co_off + 4U, 512U - co_off - 4U,
                        near_count, a64::EmitPolicy{false, true}, islands,
                        &plan) == a64::Error::None) {
            tramp_slot = std::move(far_tramp);
            island_slots[0] = std::move(entry_island);
            for (uint8_t k = 0; k != branch_count; ++k)
              island_slots[1U + k] = std::move(branch_slots[k]);
            island_count = static_cast<uint8_t>(1U + branch_count);
            entry_branch_target = entry_island_addr;
          }
        }
      }
    }
  }

  // Path D: Non-BTI 16B/20B absolute entry fallback.
  if (!tramp_slot && !bti_guarded) {
    const size_t far_count = need_landing ? 5U : 4U;
    if (far_count * 4U > effective_max_patch) {
      last_error = a64::Error::SymbolTooShort;
    } else if (!page_range_executable(target_addr, far_count * 4U)) {
      last_error = a64::Error::Capacity;
    } else if (a64::has_interior_branch(target_bytes, scan_bytes, target_addr,
                                        far_count * 4U)) {
      last_error = a64::Error::InteriorBranch;
    } else {
      const a64::EmitPolicy far_policy{true, false};
      const a64::Error analyzed_far = a64::analyze(
          target_bytes, far_count * 4U, target_addr, far_count, &plan);
      if (analyzed_far != a64::Error::None) {
        last_error = analyzed_far;
      } else {
        tramp_slot = trampoline_pool().alloc_any(
            tramp_prot,
            [&](uintptr_t hint) -> bool {
              return a64::plan(target_bytes, far_count * 4U, target_addr,
                               hint + co_off + 4U, 512U - co_off - 4U,
                               far_count, far_policy,
                               &plan) == a64::Error::None;
            },
            single_thread);
        if (tramp_slot) {
          covered_count = far_count;
          use_far_entry = true;
          entry_branch_target = reinterpret_cast<uintptr_t>(tramp_slot.get());
        } else {
          last_error =
              plan.error != a64::Error::None ? plan.error : a64::Error::Mapping;
        }
      }
    }
  }

  if (!tramp_slot)
    return reject(last_error);

  const size_t patch_size = covered_count * 4U;
  const uintptr_t tramp_addr = reinterpret_cast<uintptr_t>(tramp_slot.get());
  uint8_t staging[512]{};
  if (co_off + 4U + plan.code_size > sizeof(staging))
    return reject(a64::Error::Capacity);

  memcpy(staging, yz_cap_tmpl, cap_size);
  *reinterpret_cast<uint64_t *>(staging + ctx_off) =
      capture_return ? reinterpret_cast<uint64_t>(g_yz_ret_ctx) : 0;
  *reinterpret_cast<uint64_t *>(staging + wrap_off) =
      reinterpret_cast<uint64_t>(replacement);
  auto *co_stage = reinterpret_cast<uint32_t *>(staging + co_off);
  co_stage[0] = 0xd50324dfU; // bti jc

  out->phase = HookPhase::Prepared;
  memcpy(out->saved, target, patch_size);
  const a64::Error emitted =
      a64::emit(plan, target_addr, tramp_addr + co_off + 4U,
                staging + co_off + 4U, sizeof(staging) - co_off - 4U);
  if (emitted != a64::Error::None) {
    out->phase = HookPhase::Released;
    return reject(emitted);
  }
  if (!trampoline_pool().commit_slot(tramp_slot, staging,
                                     co_off + 4U + plan.code_size)) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::Protect);
  }

  uint32_t patch[a64::kMaxStolen] = {};
  if (!use_far_entry) {
    if (covered_count == 1U) {
      patch[0] = a64::branch(target_addr, entry_branch_target);
    } else {
      patch[0] = entry_landing(out->saved[0]);
      patch[1] = a64::branch(target_addr + 4U, entry_branch_target);
    }
  } else {
    size_t idx = 0;
    if (need_landing)
      patch[idx++] = entry_landing(out->saved[0]);
    patch[idx++] = 0x58000050U; // ldr x16, #8
    patch[idx++] = 0xd61f0200U; // br x16
    const uint64_t addr64 = static_cast<uint64_t>(tramp_addr);
    memcpy(&patch[idx], &addr64, sizeof(addr64));
  }

  size_t write_off = 0;
  size_t write_len = patch_size;
  if (patch_size == 8U && patch[0] == out->saved[0]) {
    write_off = 4U;
    write_len = 4U;
  }
  const uintptr_t write_addr = target_addr + write_off;
  const bool single_word_atomic = write_len == 4U && (write_addr & 3U) == 0U;

  if (!prepare_instruction_sync(write_addr, write_len, &out->sync_plan)) {
    out->phase = HookPhase::Released;
    return reject(a64::Error::Protect);
  }
  const uintptr_t orig_callable = tramp_addr + co_off;
  if (options.pre_publish_trampoline_slot != nullptr)
    options.pre_publish_trampoline_slot->store(orig_callable,
                                               std::memory_order_release);
  if (options.pre_publish_entry_slot != nullptr)
    options.pre_publish_entry_slot->store(orig_callable,
                                          std::memory_order_release);
  out->target = t;
  memcpy(out->patch, patch, patch_size);
  out->patched_size = static_cast<uint8_t>(patch_size);
  bool concurrent_rejected = false;
  const auto *exp_bytes =
      reinterpret_cast<const uint8_t *>(out->saved) + write_off;
  const auto *rep_bytes = reinterpret_cast<const uint8_t *>(patch) + write_off;
  const PatchResult committed = patch_bytes_checked(
      write_addr, exp_bytes, rep_bytes, write_len, single_word_atomic,
      target_addr, target_addr + patch_size, &concurrent_rejected);
  if (committed == PatchResult::Rejected) {
    out->phase = HookPhase::Released;
    out->patched_size = 0;
    return reject(concurrent_rejected ? a64::Error::ConcurrentUnsafe
                                      : a64::Error::PatchRejected);
  }
  out->trampoline = tramp_slot.release(&out->trampoline_pooled);
  out->island_count = island_count;
  for (uint8_t k = 0; k != island_count; ++k)
    out->islands[k] = island_slots[k].release(&out->island_pooled[k]);
  out->active = true;
  out->restored = false;
  out->phase = HookPhase::PublishedPendingSync;
  if (committed == PatchResult::Indeterminate ||
      synchronize_instruction_view(out->sync_plan, SyncOperation::Publish) !=
          SyncResult::Confirmed)
    fail_indeterminate(&out->phase);
  out->phase = HookPhase::Installed;
  return reinterpret_cast<void *>(tramp_addr + co_off);
}

inline bool uninstall(Hook *h, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (h == nullptr)
    return false;
  if (h->phase == HookPhase::Released)
    return true;
  if (h->phase != HookPhase::Installed &&
      h->phase != HookPhase::RestoredPendingReclaim)
    fail_indeterminate(&h->phase);
  const bool defer_reclaim = !single_threaded_process();
  if (h->phase == HookPhase::Installed) {
    if (!instruction_sync_still_supported(h->sync_plan))
      return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(h->target);
    const size_t patch_size = h->patched_size;
    size_t write_off = 0;
    size_t write_len = patch_size;
    if (patch_size == 8U && h->patch[0] == h->saved[0]) {
      write_off = 4U;
      write_len = 4U;
    }
    const uintptr_t write_addr = target + write_off;
    const bool single_word_atomic = write_len == 4U && (write_addr & 3U) == 0U;
    const bool discarded = mode == UnhookMode::DiscardCowPages &&
                           discard_cow_patch_pages(target, h->patch, h->saved,
                                                   patch_size, &h->phase);
    const auto *exp_bytes =
        reinterpret_cast<const uint8_t *>(h->patch) + write_off;
    const auto *rep_bytes =
        reinterpret_cast<const uint8_t *>(h->saved) + write_off;
    bool concurrent_rejected = false;
    const PatchResult restored =
        discarded
            ? PatchResult::Applied
            : patch_bytes_checked(write_addr, exp_bytes, rep_bytes, write_len,
                                  single_word_atomic, target,
                                  target + patch_size, &concurrent_rejected);
    if (restored == PatchResult::Rejected)
      return false;
    if (restored == PatchResult::Indeterminate)
      fail_indeterminate(&h->phase);
    h->restored = true;
    h->phase = HookPhase::RestoredPendingSync;
    if (synchronize_instruction_view(h->sync_plan, SyncOperation::Restore) !=
        SyncResult::Confirmed)
      fail_indeterminate(&h->phase);
    h->phase = HookPhase::RestoredPendingReclaim;
  }
  trampoline_pool().retire_slot(h->trampoline, h->trampoline_pooled,
                                defer_reclaim);
  h->trampoline = nullptr;
  h->trampoline_pooled = false;
  for (uint8_t k = 0; k != h->island_count; ++k) {
    island_pool().retire_slot(h->islands[k], h->island_pooled[k],
                              defer_reclaim);
    h->islands[k] = nullptr;
    h->island_pooled[k] = false;
  }
  h->island_count = 0;
  h->patched_size = 0;
  h->active = false;
  h->phase = HookPhase::Released;
  return true;
}

} // namespace yuki::ihook
#elif defined(__arm__)
extern "C" {
extern uint8_t yz_cap_arm_tmpl[];
extern uint8_t yz_cap_arm_tmpl_ctx[];
extern uint8_t yz_cap_arm_tmpl_wrap[];
extern uint8_t yz_cap_arm_tmpl_end[];
extern uint8_t yz_cap_thumb_tmpl[];
extern uint8_t yz_cap_thumb_tmpl_ctx[];
extern uint8_t yz_cap_thumb_tmpl_wrap[];
extern uint8_t yz_cap_thumb_tmpl_end[];
extern uint32_t g_yz_ret_ctx[];
bool yz_patch_text(uintptr_t addr, const void *bytes, unsigned int len);
}

namespace yuki::ihook {

struct Hook {
  uint32_t *target = nullptr;
  uint8_t saved[arm32::kMaxCoveredBytes] = {};
  uint8_t patch[12] = {};
  void *trampoline = nullptr;
  void *island = nullptr;
  bool trampoline_pooled = false;
  bool island_pooled = false;
  bool thumb = false;
  uint8_t patched_size = 0;
  uint8_t saved_size = 0;
  bool active = false;
  bool restored = false;
  HookPhase phase = HookPhase::Released;
  InstructionSyncPlan sync_plan{};
};

inline size_t absolute_jump_size(uintptr_t instruction_address, bool thumb) {
  return thumb && (instruction_address & 2U) != 0 ? 10U : 8U;
}

inline void emit_relative_jump(uint8_t *where, uintptr_t from, uintptr_t to,
                               bool thumb) {
  if (!thumb) {
    uint32_t branch = 0;
    (void)arm32::detail::encode_a32_branch(static_cast<uint32_t>(from),
                                           static_cast<uint32_t>(to), 0xeU,
                                           false, &branch);
    memcpy(where, &branch, sizeof(branch));
    return;
  }
  uint16_t hw1 = 0, hw2 = 0;
  (void)arm32::detail::encode_t32_b_t4(static_cast<uint32_t>(from),
                                       static_cast<uint32_t>(to), &hw1, &hw2);
  const uint16_t branch[2] = {hw1, hw2};
  memcpy(where, branch, sizeof(branch));
}

inline size_t emit_absolute_jump(uint8_t *where, uintptr_t instruction_address,
                                 uintptr_t destination, bool thumb) {
  if (thumb) {
    const bool needs_padding = (instruction_address & 2U) != 0;
    size_t offset = 0;
    if (needs_padding) {
      const uint16_t nop = 0xbf00U;
      memcpy(where, &nop, sizeof(nop));
      offset += sizeof(nop);
    }
    const uint16_t jump[2] = {0xf8dfU, 0xf000U}; // ldr.w pc, [pc, #0]
    memcpy(where + offset, jump, sizeof(jump));
    offset += sizeof(jump);
    const uint32_t address = static_cast<uint32_t>(destination);
    memcpy(where + offset, &address, sizeof(address));
    return offset + sizeof(address);
  }
  const uint32_t jump = 0xe51ff004U; // ldr pc, [pc, #-4]
  memcpy(where, &jump, sizeof(jump));
  const uint32_t address = static_cast<uint32_t>(destination);
  memcpy(where + 4U, &address, sizeof(address));
  return 8U;
}

inline const uint8_t *arm32_code_bytes(const uint8_t *symbol) {
  return reinterpret_cast<const uint8_t *>(reinterpret_cast<uintptr_t>(symbol) &
                                           ~uintptr_t{1});
}

inline TrampolineSlot alloc_arm32_near(uintptr_t target_address,
                                       const uint8_t *source, size_t available,
                                       size_t original_offset, bool thumb,
                                       arm32::Plan *plan,
                                       bool allow_mprotect_reuse) {
  const uintptr_t reach = thumb ? 0x00fffff0U : 0x01fffff0U;
  const size_t page_size = static_cast<size_t>(getpagesize());
  const uintptr_t min_addr =
      target_address > reach ? target_address - reach : page_size;
  const uintptr_t max_addr = target_address <= UINT32_MAX - reach
                                 ? target_address + reach
                                 : UINT32_MAX;
  return trampoline_pool().alloc_near(
      min_addr, max_addr, target_address, PROT_READ | PROT_EXEC,
      [&](uintptr_t hint) -> bool {
        if (hint > UINT32_MAX - 512U)
          return false;
        const bool jump_ok =
            thumb
                ? arm32::t32_b_reachable(static_cast<uint32_t>(target_address),
                                         static_cast<uint32_t>(hint))
                : arm32::a32_b_reachable(static_cast<uint32_t>(target_address),
                                         static_cast<uint32_t>(hint));
        if (!jump_ok)
          return false;
        return arm32::plan(source, available, target_address,
                           hint + original_offset, 512U - original_offset, 4U,
                           thumb, plan) == arm32::Error::None;
      },
      allow_mprotect_reuse);
}

inline IslandSlot alloc_arm32_entry_island(uintptr_t target_address,
                                           uintptr_t tramp_callable, bool thumb,
                                           bool allow_mprotect_reuse) {
  const uintptr_t reach = thumb ? 0x00fffff0U : 0x01fffff0U;
  const size_t page_size = static_cast<size_t>(getpagesize());
  const uintptr_t min_addr =
      target_address > reach ? target_address - reach : page_size;
  const uintptr_t max_addr = target_address <= UINT32_MAX - reach
                                 ? target_address + reach
                                 : UINT32_MAX;
  IslandSlot slot = island_pool().alloc_near(
      min_addr, max_addr, target_address, PROT_READ | PROT_EXEC,
      [&](uintptr_t hint) -> bool {
        if (hint > UINT32_MAX - 16U)
          return false;
        return thumb ? arm32::t32_b_reachable(
                           static_cast<uint32_t>(target_address),
                           static_cast<uint32_t>(hint))
                     : arm32::a32_b_reachable(
                           static_cast<uint32_t>(target_address),
                           static_cast<uint32_t>(hint));
      },
      allow_mprotect_reuse);
  if (!slot)
    return {};
  uint8_t buf[16]{};
  const size_t bytes = arm32::emit_entry_island(
      buf, sizeof(buf), reinterpret_cast<uintptr_t>(slot.get()), tramp_callable,
      thumb);
  if (bytes == 0 || !island_pool().commit_slot(slot, buf, bytes))
    return {};
  return slot;
}

inline void *install(void *target, void *replacement, Hook *out,
                     bool prefer_relative = false, bool capture_return = false,
                     arm32::Error *reason = nullptr, size_t max_patch_size = 10,
                     InstallOptions options = {}) {
  const auto reject = [reason](arm32::Error error) -> void * {
    if (reason != nullptr)
      *reason = error;
    return nullptr;
  };
  if (reason != nullptr)
    *reason = arm32::Error::None;
  if (target == nullptr || replacement == nullptr || out == nullptr ||
      out->active || out->phase != HookPhase::Released || max_patch_size < 4U)
    return reject(arm32::Error::InvalidInput);

  uintptr_t callable = reinterpret_cast<uintptr_t>(target);
  const size_t page_size = static_cast<size_t>(getpagesize());
  if (options.follow_thunk) {
    for (unsigned hop = 0; hop != 4U; ++hop) {
      const bool cur_thumb = (callable & 1U) != 0;
      const uintptr_t raw = callable & ~uintptr_t{1};
      const size_t avail = page_size - (raw % page_size);
      uintptr_t next = 0;
      if (!arm32::resolve_thunk(reinterpret_cast<const uint8_t *>(raw), avail,
                                raw, cur_thumb, &next))
        break;
      const uintptr_t next_raw = next & ~uintptr_t{1};
      if (next_raw == 0 || next_raw == raw ||
          !bounds_detail::exec_addr_readable(next_raw, 4U))
        break;
      callable = next;
    }
  }

  const bool thumb = (callable & 1U) != 0;
  const uintptr_t target_address = callable & ~uintptr_t{1};
  if (!thumb && (target_address & 3U) != 0)
    return reject(arm32::Error::InvalidInput);
  if (thumb && arm32::thumb_inside_it(target_address, page_size))
    return reject(arm32::Error::Unsupported);

  TargetBounds bounds{};
  query_target_bounds(target_address, &bounds);
  size_t effective_max_patch = max_patch_size;
  if (bounds.have_vma && bounds.vma_remaining < effective_max_patch)
    effective_max_patch = bounds.vma_remaining;
  if (bounds.have_symbol_size && bounds.symbol_size < effective_max_patch)
    effective_max_patch = bounds.symbol_size;
  if (effective_max_patch < 4U)
    return reject(arm32::Error::SymbolTooShort);

  const uint8_t *capture =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl : yz_cap_arm_tmpl);
  const uint8_t *capture_end =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_end : yz_cap_arm_tmpl_end);
  const uint8_t *capture_ctx =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_ctx : yz_cap_arm_tmpl_ctx);
  const uint8_t *capture_wrap =
      arm32_code_bytes(thumb ? yz_cap_thumb_tmpl_wrap : yz_cap_arm_tmpl_wrap);
  const auto capture_address = reinterpret_cast<uintptr_t>(capture);
  const size_t capture_size =
      reinterpret_cast<uintptr_t>(capture_end) - capture_address;
  const size_t call_original_offset = (capture_size + 3U) & ~size_t{3};
  if (call_original_offset >= 512U - 64U)
    return reject(arm32::Error::Capacity);

  const auto *target_bytes = reinterpret_cast<const uint8_t *>(target_address);
  const size_t page_remaining = page_size - (target_address % page_size);
  const bool next_page_exec =
      page_remaining < arm32::kMaxCoveredBytes &&
      page_range_executable(target_address, arm32::kMaxCoveredBytes);
  const size_t max_readable =
      next_page_exec ? page_remaining + page_size : page_remaining;
  size_t symbol_limit = max_readable;
  if (bounds.have_vma && bounds.vma_remaining < symbol_limit)
    symbol_limit = bounds.vma_remaining;
  if (bounds.have_symbol_size && bounds.symbol_size < symbol_limit)
    symbol_limit = bounds.symbol_size;
  const size_t scan_bytes = symbol_limit;
  const bool single_thread = single_threaded_process();

  arm32::Plan plan{};
  TrampolineSlot slot;
  IslandSlot island_slot;
  size_t patch_size = 0;
  bool relative = false;
  arm32::Error last_error = arm32::Error::Mapping;

  if (prefer_relative && page_range_executable(target_address, 4U)) {
    const arm32::Error analyzed = arm32::analyze(
        target_bytes, max_readable, target_address, 4U, thumb, &plan);
    if (analyzed != arm32::Error::None) {
      last_error = analyzed;
    } else if (plan.copied_size > symbol_limit) {
      last_error = arm32::Error::SymbolTooShort;
    } else if (arm32::has_interior_branch(target_bytes, scan_bytes,
                                          target_address, plan.copied_size,
                                          thumb)) {
      last_error = arm32::Error::InteriorBranch;
    } else {
      // Path A: Near pooled trampoline.
      slot =
          alloc_arm32_near(target_address, target_bytes, max_readable,
                           call_original_offset, thumb, &plan, single_thread);
      if (slot) {
        patch_size = 4U;
        relative = true;
      } else {
        // Path B: Near ARM32 Entry Island + far pooled trampoline.
        TrampolineSlot far_tramp = trampoline_pool().alloc_any(
            PROT_READ | PROT_EXEC,
            [&](uintptr_t hint) -> bool {
              if (hint > UINT32_MAX - 512U)
                return false;
              return arm32::plan(target_bytes, max_readable, target_address,
                                 hint + call_original_offset,
                                 512U - call_original_offset, 4U, thumb,
                                 &plan) == arm32::Error::None;
            },
            single_thread);
        if (far_tramp) {
          const uintptr_t far_callable =
              reinterpret_cast<uintptr_t>(far_tramp.get()) + (thumb ? 1U : 0U);
          IslandSlot entry_isl = alloc_arm32_entry_island(
              target_address, far_callable, thumb, single_thread);
          if (entry_isl) {
            slot = std::move(far_tramp);
            island_slot = std::move(entry_isl);
            patch_size = 4U;
            relative = true;
          }
        }
      }
    }
  }

  if (!slot && last_error != arm32::Error::InteriorBranch) {
    patch_size = absolute_jump_size(target_address, thumb);
    if (patch_size > effective_max_patch)
      return reject(arm32::Error::SymbolTooShort);
    if (!page_range_executable(target_address, patch_size))
      return reject(arm32::Error::Capacity);
    const arm32::Error analyzed = arm32::analyze(
        target_bytes, max_readable, target_address, patch_size, thumb, &plan);
    if (analyzed != arm32::Error::None)
      return reject(analyzed);
    if (plan.copied_size > symbol_limit)
      return reject(arm32::Error::SymbolTooShort);
    if (arm32::has_interior_branch(target_bytes, scan_bytes, target_address,
                                   plan.copied_size, thumb))
      return reject(arm32::Error::InteriorBranch);
    slot = trampoline_pool().alloc_any(
        PROT_READ | PROT_EXEC,
        [&](uintptr_t hint) -> bool {
          return arm32::plan(target_bytes, max_readable, target_address,
                             hint + call_original_offset,
                             512U - call_original_offset, patch_size, thumb,
                             &plan) == arm32::Error::None;
        },
        single_thread);
    if (!slot)
      return reject(plan.error != arm32::Error::None ? plan.error
                                                     : arm32::Error::Mapping);
  }

  if (!slot)
    return reject(last_error);

  if (plan.copied_size > sizeof(out->saved) ||
      !page_range_executable(target_address, plan.copied_size)) {
    return reject(arm32::Error::Capacity);
  }

  const size_t trampoline_size = call_original_offset + plan.code_size;
  uint8_t staging[512]{};
  if (trampoline_size > sizeof(staging))
    return reject(arm32::Error::Capacity);

  out->phase = HookPhase::Prepared;
  memcpy(staging, capture, capture_size);
  const uint32_t context =
      capture_return
          ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_yz_ret_ctx))
          : 0;
  const uint32_t wrapper =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(replacement));
  memcpy(staging + (reinterpret_cast<uintptr_t>(capture_ctx) - capture_address),
         &context, sizeof(context));
  memcpy(staging +
             (reinterpret_cast<uintptr_t>(capture_wrap) - capture_address),
         &wrapper, sizeof(wrapper));

  const uintptr_t base_addr = reinterpret_cast<uintptr_t>(slot.get());
  const arm32::Error emitted = arm32::emit(
      plan, target_address, base_addr + call_original_offset,
      staging + call_original_offset, sizeof(staging) - call_original_offset);
  if (emitted != arm32::Error::None) {
    out->phase = HookPhase::Released;
    return reject(emitted);
  }
  if (!trampoline_pool().commit_slot(slot, staging, trampoline_size)) {
    out->phase = HookPhase::Released;
    return reject(arm32::Error::Protect);
  }

  uint8_t patch[12] = {};
  if (relative) {
    const uintptr_t branch_dst =
        island_slot ? reinterpret_cast<uintptr_t>(island_slot.get())
                    : base_addr;
    emit_relative_jump(patch, target_address, branch_dst, thumb);
  } else {
    emit_absolute_jump(patch, target_address, base_addr + (thumb ? 1U : 0U),
                       thumb);
  }
  memcpy(out->saved, target_bytes, plan.copied_size);
  if (!prepare_instruction_sync(target_address, patch_size, &out->sync_plan)) {
    out->phase = HookPhase::Released;
    return reject(arm32::Error::Protect);
  }
  const uintptr_t orig_callable =
      base_addr + call_original_offset + (thumb ? 1U : 0U);
  if (options.pre_publish_trampoline_slot != nullptr)
    options.pre_publish_trampoline_slot->store(orig_callable,
                                               std::memory_order_release);
  if (options.pre_publish_entry_slot != nullptr)
    options.pre_publish_entry_slot->store(orig_callable,
                                          std::memory_order_release);
  out->target = reinterpret_cast<uint32_t *>(target_address);
  memcpy(out->patch, patch, patch_size);
  out->patched_size = static_cast<uint8_t>(patch_size);
  out->saved_size = plan.copied_size;
  out->thumb = thumb;
  const bool single_word_atomic =
      !thumb && patch_size == 4U && (target_address & 3U) == 0U;
  bool concurrent_rejected = false;
  const PatchResult committed = patch_bytes_checked(
      target_address, out->saved, patch, patch_size, single_word_atomic,
      target_address, target_address + patch_size, &concurrent_rejected);
  if (committed == PatchResult::Rejected) {
    out->phase = HookPhase::Released;
    out->patched_size = 0;
    out->saved_size = 0;
    return reject(concurrent_rejected ? arm32::Error::ConcurrentUnsafe
                                      : arm32::Error::PatchRejected);
  }
  out->trampoline = slot.release(&out->trampoline_pooled);
  if (island_slot)
    out->island = island_slot.release(&out->island_pooled);
  out->active = true;
  out->restored = false;
  out->phase = HookPhase::PublishedPendingSync;
  if (committed == PatchResult::Indeterminate ||
      synchronize_instruction_view(out->sync_plan, SyncOperation::Publish) !=
          SyncResult::Confirmed)
    fail_indeterminate(&out->phase);
  out->phase = HookPhase::Installed;
  return reinterpret_cast<void *>(base_addr + call_original_offset +
                                  (thumb ? 1U : 0U));
}

inline bool uninstall(Hook *hook, UnhookMode mode = UnhookMode::RestoreBytes) {
  if (hook == nullptr)
    return false;
  if (hook->phase == HookPhase::Released)
    return true;
  if (hook->phase != HookPhase::Installed &&
      hook->phase != HookPhase::RestoredPendingReclaim)
    fail_indeterminate(&hook->phase);
  const bool defer_reclaim = !single_threaded_process();
  if (hook->phase == HookPhase::Installed) {
    if (!instruction_sync_still_supported(hook->sync_plan))
      return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(hook->target);
    const bool discarded =
        mode == UnhookMode::DiscardCowPages &&
        discard_cow_patch_pages(target, hook->patch, hook->saved,
                                hook->patched_size, &hook->phase);
    const bool single_word_atomic =
        !hook->thumb && hook->patched_size == 4U && (target & 3U) == 0U;
    bool concurrent_rejected = false;
    const PatchResult restored =
        discarded ? PatchResult::Applied
                  : patch_bytes_checked(target, hook->patch, hook->saved,
                                        hook->patched_size, single_word_atomic,
                                        target, target + hook->patched_size,
                                        &concurrent_rejected);
    if (restored == PatchResult::Rejected)
      return false;
    if (restored == PatchResult::Indeterminate)
      fail_indeterminate(&hook->phase);
    hook->restored = true;
    hook->phase = HookPhase::RestoredPendingSync;
    if (synchronize_instruction_view(hook->sync_plan, SyncOperation::Restore) !=
        SyncResult::Confirmed)
      fail_indeterminate(&hook->phase);
    hook->phase = HookPhase::RestoredPendingReclaim;
  }
  trampoline_pool().retire_slot(hook->trampoline, hook->trampoline_pooled,
                                defer_reclaim);
  hook->trampoline = nullptr;
  hook->trampoline_pooled = false;
  island_pool().retire_slot(hook->island, hook->island_pooled, defer_reclaim);
  hook->island = nullptr;
  hook->island_pooled = false;
  hook->patched_size = 0;
  hook->saved_size = 0;
  hook->thumb = false;
  hook->active = false;
  hook->phase = HookPhase::Released;
  return true;
}

} // namespace yuki::ihook
#else
namespace yuki::ihook {

struct Hook {
  bool active = false;
  uint8_t patched_size = 0;
};

inline void *install(void *, void *, Hook *, bool = false, bool = false) {
  return nullptr;
}
inline bool uninstall(Hook *, UnhookMode = UnhookMode::RestoreBytes) {
  return false;
}

} // namespace yuki::ihook
#endif // #if defined(__aarch64__)
