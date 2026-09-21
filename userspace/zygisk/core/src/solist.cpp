#include "solist.hpp"
#include "log.hpp"

#include <climits>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif // #ifndef PR_SET_VMA
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif // #ifndef PR_SET_VMA_ANON_NAME

namespace yuki::solist {
namespace {

#define SLOGE(...) ZLOGE(__VA_ARGS__)
#define SLOGI(...) ZLOGI(__VA_ARGS__)

constexpr int kMaxWalk = 2000; /* guard against a wrong offset / cyclic list */
/* Android bionic CFI shadow format. */
constexpr uintptr_t kCfiShadowGranularity = 18;
constexpr uintptr_t kCfiShadowEntrySize = sizeof(uint16_t);

inline uintptr_t elf_runtime_address(ElfW(Addr) load_bias, ElfW(Addr) value) {
  return static_cast<uintptr_t>(load_bias + value);
}

inline uintptr_t elf_load_bias(uintptr_t mapped, ElfW(Addr) value) {
  return static_cast<uintptr_t>(static_cast<ElfW(Addr)>(mapped) - value);
}

using realpath_fn = const char *(*)(void *);
using guard_fn = void (*)(void *);

size_t page_size() {
  long sz = sysconf(_SC_PAGESIZE);
  return sz > 0 ? static_cast<size_t>(sz) : 4096;
}

inline uintptr_t page_down(uintptr_t addr, size_t pg) {
  return addr & ~(static_cast<uintptr_t>(pg) - 1);
}
inline uintptr_t page_up(uintptr_t addr, size_t pg) {
  return (addr + pg - 1) & ~(static_cast<uintptr_t>(pg) - 1);
}

class LinkerSyms {
public:
  LinkerSyms() = default;
  LinkerSyms(const LinkerSyms &) = delete;
  LinkerSyms &operator=(const LinkerSyms &) = delete;
  LinkerSyms(LinkerSyms &&) = delete;
  LinkerSyms &operator=(LinkerSyms &&) = delete;

  bool init() {
    if (!find_linker_base())
      return false;
    if (!map_and_parse())
      return false;
    return symtab_ != nullptr && strtab_ != nullptr;
  }

  ~LinkerSyms() {
    if (map_ != MAP_FAILED && map_ != nullptr)
      munmap(map_, map_sz_);
  }

  uintptr_t find(const char *prefix) const {
    const size_t plen = strlen(prefix);
    for (size_t i = 0; i < sym_cnt_; ++i) {
      const ElfW(Sym) &s = symtab_[i];
      if (s.st_name == 0 || s.st_name >= strtab_sz_ || s.st_value == 0 ||
          s.st_shndx == SHN_UNDEF)
        continue;
      const char *name = strtab_ + s.st_name;
      if (memchr(name, '\0', strtab_sz_ - s.st_name) == nullptr ||
          strncmp(name, prefix, plen) != 0)
        continue;
      uintptr_t address =
          elf_runtime_address(static_cast<ElfW(Addr)>(load_bias_), s.st_value);
#if defined(__arm__)
      uintptr_t mapped_address = address & ~static_cast<uintptr_t>(1);
#else
      uintptr_t mapped_address = address;
#endif
      if (runtime_contains(mapped_address))
        return address;
    }
    return 0;
  }

private:
  static bool file_range_valid(uint64_t offset, uint64_t size,
                               size_t file_size) {
    return offset <= file_size && size <= file_size - offset;
  }

  static bool valid_ident(const unsigned char *ident) {
    return memcmp(ident, ELFMAG, SELFMAG) == 0 &&
           ident[EI_CLASS] == (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) &&
           ident[EI_DATA] == ELFDATA2LSB && ident[EI_VERSION] == EV_CURRENT;
  }

  static bool valid_machine(ElfW(Half) machine) {
#if defined(__aarch64__)
    return machine == EM_AARCH64;
#elif defined(__arm__)
    return machine == EM_ARM;
#else
    (void)machine;
    return false;
#endif
  }

  [[nodiscard]] bool runtime_contains(uintptr_t address) const {
    for (size_t i = 0; i < runtime_phnum_; ++i) {
      const ElfW(Phdr) &ph = runtime_phdr_[i];
      if (ph.p_type != PT_LOAD)
        continue;
      uintptr_t start =
          elf_runtime_address(static_cast<ElfW(Addr)>(load_bias_), ph.p_vaddr);
      if (ph.p_memsz > UINTPTR_MAX - start)
        continue;
      if (address >= start && address < start + ph.p_memsz)
        return true;
    }
    return false;
  }

  bool runtime_phdr_matches(const ElfW(Ehdr) * eh,
                            const ElfW(Phdr) * phdr) const {
    if (eh->e_phnum != runtime_phnum_)
      return false;
    for (size_t i = 0; i < runtime_phnum_; ++i) {
      const ElfW(Phdr) &a = runtime_phdr_[i];
      const ElfW(Phdr) &b = phdr[i];
      if (a.p_type != b.p_type || a.p_flags != b.p_flags ||
          a.p_offset != b.p_offset || a.p_vaddr != b.p_vaddr ||
          a.p_filesz != b.p_filesz || a.p_memsz != b.p_memsz ||
          a.p_align != b.p_align)
        return false;
    }
    return true;
  }

  bool find_linker_base() {
    constexpr uint64_t kMaxProgramHeaderBytes = 64ULL * 1024ULL;
    uintptr_t linker_base = static_cast<uintptr_t>(getauxval(AT_BASE));
    if (linker_base == 0)
      return false;

    const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(linker_base);
    if (!valid_ident(eh->e_ident) || !valid_machine(eh->e_machine) ||
        eh->e_type != ET_DYN || eh->e_version != EV_CURRENT ||
        eh->e_ehsize != sizeof(ElfW(Ehdr)) ||
        eh->e_phentsize != sizeof(ElfW(Phdr)) || eh->e_phnum == 0 ||
        eh->e_phnum > 128 || eh->e_phoff > kMaxProgramHeaderBytes ||
        static_cast<uint64_t>(eh->e_phnum) * sizeof(ElfW(Phdr)) >
            kMaxProgramHeaderBytes - eh->e_phoff ||
        eh->e_phoff > UINTPTR_MAX - linker_base)
      return false;

    runtime_phdr_ =
        reinterpret_cast<const ElfW(Phdr) *>(linker_base + eh->e_phoff);
    runtime_phnum_ = eh->e_phnum;
    for (size_t i = 0; i < runtime_phnum_; ++i) {
      const ElfW(Phdr) &ph = runtime_phdr_[i];
      if (ph.p_type != PT_LOAD)
        continue;
      uintptr_t mapped =
          elf_runtime_address(static_cast<ElfW(Addr)>(linker_base),
                              static_cast<ElfW(Addr)>(ph.p_offset));
      load_bias_ = elf_load_bias(mapped, ph.p_vaddr);
      break;
    }
    if (load_bias_ == 0)
      return false;

    uintptr_t main_phdr_addr = static_cast<uintptr_t>(getauxval(AT_PHDR));
    size_t main_phnum = static_cast<size_t>(getauxval(AT_PHNUM));
    size_t main_phent = static_cast<size_t>(getauxval(AT_PHENT));
    if (main_phdr_addr == 0 || main_phnum == 0 || main_phnum > 128 ||
        main_phent != sizeof(ElfW(Phdr)))
      return false;

    const auto *main_phdr =
        reinterpret_cast<const ElfW(Phdr) *>(main_phdr_addr);
    uintptr_t main_bias = 0;
    bool have_main_bias = false;
    const ElfW(Phdr) *interp_phdr = nullptr;
    for (size_t i = 0; i < main_phnum; ++i) {
      if (main_phdr[i].p_type == PT_PHDR) {
        main_bias = elf_load_bias(main_phdr_addr, main_phdr[i].p_vaddr);
        have_main_bias = true;
      } else if (main_phdr[i].p_type == PT_INTERP) {
        interp_phdr = &main_phdr[i];
      }
    }
    if (!have_main_bias || interp_phdr == nullptr ||
        interp_phdr->p_filesz < 2 || interp_phdr->p_filesz > sizeof(path_))
      return false;

    bool interp_mapped = false;
    for (size_t i = 0; i < main_phnum; ++i) {
      const ElfW(Phdr) &ph = main_phdr[i];
      if (ph.p_type != PT_LOAD || interp_phdr->p_vaddr < ph.p_vaddr)
        continue;
      uint64_t relative = interp_phdr->p_vaddr - ph.p_vaddr;
      if (relative <= ph.p_memsz &&
          interp_phdr->p_filesz <= ph.p_memsz - relative) {
        interp_mapped = true;
        break;
      }
    }
    if (!interp_mapped)
      return false;

    const char *interp = reinterpret_cast<const char *>(elf_runtime_address(
        static_cast<ElfW(Addr)>(main_bias), interp_phdr->p_vaddr));
    size_t path_len = strnlen(interp, interp_phdr->p_filesz);
    if (path_len == 0 || path_len >= interp_phdr->p_filesz ||
        path_len >= sizeof(path_) || interp[0] != '/')
      return false;
    memcpy(path_, interp, path_len + 1);
    return true;
  }

  bool map_and_parse() {
    int fd = open(path_, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) < sizeof(ElfW(Ehdr)) ||
        static_cast<uint64_t>(st.st_size) > SIZE_MAX) {
      close(fd);
      return false;
    }
    map_sz_ = (size_t)st.st_size;
    map_ = mmap(nullptr, map_sz_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map_ == MAP_FAILED)
      return false;

    const auto *base = static_cast<const uint8_t *>(map_);
    const auto *eh = reinterpret_cast<const ElfW(Ehdr) *>(base);
    if (!valid_ident(eh->e_ident) || !valid_machine(eh->e_machine) ||
        eh->e_type != ET_DYN || eh->e_version != EV_CURRENT ||
        eh->e_ehsize != sizeof(ElfW(Ehdr)) ||
        eh->e_phentsize != sizeof(ElfW(Phdr)) || eh->e_phnum == 0 ||
        eh->e_phnum > 128 ||
        !file_range_valid(
            eh->e_phoff,
            static_cast<uint64_t>(eh->e_phnum) * sizeof(ElfW(Phdr)), map_sz_) ||
        eh->e_shentsize != sizeof(ElfW(Shdr)) || eh->e_shnum == 0 ||
        !file_range_valid(
            eh->e_shoff,
            static_cast<uint64_t>(eh->e_shnum) * sizeof(ElfW(Shdr)), map_sz_))
      return false;

    const auto *ph = reinterpret_cast<const ElfW(Phdr) *>(base + eh->e_phoff);
    if (!runtime_phdr_matches(eh, ph))
      return false;

    const auto *sh = reinterpret_cast<const ElfW(Shdr) *>(base + eh->e_shoff);
    for (size_t i = 0; i < eh->e_shnum; ++i) {
      if (sh[i].sh_type != SHT_SYMTAB)
        continue;
      if (sh[i].sh_link >= eh->e_shnum ||
          sh[i].sh_entsize != sizeof(ElfW(Sym)) ||
          sh[i].sh_size % sizeof(ElfW(Sym)) != 0 ||
          !file_range_valid(sh[i].sh_offset, sh[i].sh_size, map_sz_))
        return false;
      const ElfW(Shdr) &str = sh[sh[i].sh_link];
      if (str.sh_type != SHT_STRTAB || str.sh_size == 0 ||
          !file_range_valid(str.sh_offset, str.sh_size, map_sz_))
        return false;
      symtab_ = reinterpret_cast<const ElfW(Sym) *>(base + sh[i].sh_offset);
      sym_cnt_ = sh[i].sh_size / sizeof(ElfW(Sym));
      strtab_ = reinterpret_cast<const char *>(base + str.sh_offset);
      strtab_sz_ = str.sh_size;
      return strtab_[0] == '\0' && strtab_[strtab_sz_ - 1] == '\0';
    }
    return false; /* stripped .symtab -- give up */
  }

  uintptr_t load_bias_ = 0;
  const ElfW(Phdr) *runtime_phdr_ = nullptr;
  size_t runtime_phnum_ = 0;
  char path_[PATH_MAX] = {};
  void *map_ = MAP_FAILED;
  size_t map_sz_ = 0;
  const ElfW(Sym) *symtab_ = nullptr;
  const char *strtab_ = nullptr;
  size_t strtab_sz_ = 0;
  size_t sym_cnt_ = 0;
};

/* Runtime soinfo unload glue. */
size_t g_size_off = 0, g_next_off = 0, g_ctor_off = 0;
void (*g_soinfo_unload)(void *) = nullptr;
size_t *g_load_counter = nullptr;
size_t *g_unload_counter = nullptr;
realpath_fn g_realpath_u = nullptr;
realpath_fn g_soname_u = nullptr;
guard_fn g_pdg_ctor_u = nullptr, g_pdg_dtor_u = nullptr;
void *g_solist_head = nullptr;
void **g_solist_head_slot = nullptr;
bool g_unload_done = false, g_unload_ok = false;

constexpr size_t kSizeBlockRange = 1024; /* bytes scanned for soinfo fields */
constexpr size_t kSizeMax = 0x100000;
constexpr size_t kSizeMin = 0x100;

inline void *u_next(void *si) {
  return *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(si) +
                                    g_next_off);
}
inline void u_set_next(void *si, void *next) {
  *reinterpret_cast<void **>(reinterpret_cast<uintptr_t>(si) + g_next_off) =
      next;
}
inline size_t u_size(void *si) {
  return *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) +
                                     g_size_off);
}
inline void u_set_size(void *si, size_t v) {
  *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) + g_size_off) = v;
}
inline void u_set_ctor(void *si, bool v) {
  *reinterpret_cast<size_t *>(reinterpret_cast<uintptr_t>(si) + g_ctor_off) =
      v ? 1 : 0;
}

/* Probe soinfo field offsets. */
bool u_probe_offsets(void *somain, void *solinker, void *vdso,
                     const char *linker_path) {
  bool size_ok = false, next_ok = false, ctor_ok = false;
  for (size_t i = 0; i < kSizeBlockRange / sizeof(void *); ++i) {
    if (!size_ok) {
      size_t v = *reinterpret_cast<size_t *>(
          reinterpret_cast<uintptr_t>(somain) + (i * sizeof(void *)));
      if (v > kSizeMin && v < kSizeMax) {
        g_size_off = i * sizeof(void *);
        size_ok = true;
        continue;
      }
    }
    if (!size_ok)
      continue;
    uintptr_t field =
        reinterpret_cast<uintptr_t>(solinker) + (i * sizeof(void *));
    if (!next_ok) {
      void *nx = *reinterpret_cast<void **>(field);
      if (nx == somain || (vdso != nullptr && nx == vdso)) {
        g_next_off = i * sizeof(void *);
        next_ok = true;
        continue;
      }
    }
    if (!next_ok)
      continue;
    if (!ctor_ok) {
      auto *lm = reinterpret_cast<link_map *>(field);
      size_t gap = (sizeof(link_map) + sizeof(void *) - 1) / sizeof(void *);
      uintptr_t fwd = field + (gap * sizeof(void *));
      if (*reinterpret_cast<bool *>(fwd) && lm->l_addr != 0 &&
          lm->l_name != nullptr && strcmp(linker_path, lm->l_name) == 0) {
        g_ctor_off = fwd - reinterpret_cast<uintptr_t>(solinker);
        ctor_ok = true;
        i += gap;
        continue;
      }
    }
  }
  return size_ok && next_ok && ctor_ok;
}

/* Resolve linker symbols and offsets. */
bool u_init() {
  if (g_unload_done)
    return g_unload_ok;
  g_unload_done = true;

  LinkerSyms syms;
  if (!syms.init())
    return false;

  uintptr_t head_var = syms.find("__dl__ZL8solinker");
  if (head_var == 0)
    head_var = syms.find("__dl__ZL6solist");
  uintptr_t somain_var = syms.find("__dl__ZL6somain");
  uintptr_t vdso_var = syms.find("__dl__ZL4vdso");
  g_soinfo_unload = reinterpret_cast<void (*)(void *)>(
      syms.find("__dl__ZL13soinfo_unloadP6soinfo"));
  g_load_counter =
      reinterpret_cast<size_t *>(syms.find("__dl__ZL21g_module_load_counter"));
  g_unload_counter = reinterpret_cast<size_t *>(
      syms.find("__dl__ZL23g_module_unload_counter"));
  g_realpath_u = reinterpret_cast<realpath_fn>(
      syms.find("__dl__ZNK6soinfo12get_realpathEv"));
  g_soname_u = reinterpret_cast<realpath_fn>(
      syms.find("__dl__ZNK6soinfo10get_sonameEv"));
  g_pdg_ctor_u =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardC2Ev"));
  if (g_pdg_ctor_u == nullptr)
    g_pdg_ctor_u = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardC1Ev"));
  g_pdg_dtor_u =
      reinterpret_cast<guard_fn>(syms.find("__dl__ZN18ProtectedDataGuardD2Ev"));
  if (g_pdg_dtor_u == nullptr)
    g_pdg_dtor_u = reinterpret_cast<guard_fn>(
        syms.find("__dl__ZN18ProtectedDataGuardD1Ev"));

  if (head_var == 0 || somain_var == 0 || g_soinfo_unload == nullptr ||
      g_realpath_u == nullptr || g_pdg_ctor_u == nullptr ||
      g_pdg_dtor_u == nullptr) {
    SLOGE("solist-unload: missing syms (head=%d somain=%d unload=%d rp=%d "
          "guard=%d)",
          head_var != 0, somain_var != 0, g_soinfo_unload != nullptr,
          g_realpath_u != nullptr,
          g_pdg_ctor_u != nullptr && g_pdg_dtor_u != nullptr);
    return false;
  }

  g_solist_head_slot = reinterpret_cast<void **>(head_var);
  g_solist_head = *g_solist_head_slot;
  void *somain = *reinterpret_cast<void **>(somain_var);
  void *vdso = vdso_var != 0 ? *reinterpret_cast<void **>(vdso_var) : nullptr;
  if (g_solist_head == nullptr || somain == nullptr)
    return false;

  const char *hp = g_realpath_u(g_solist_head);
  if (hp == nullptr || strstr(hp, "linker") == nullptr) {
    SLOGE("solist-unload: head realpath '%s' not linker-like",
          hp != nullptr ? hp : "(null)");
    return false;
  }

  if (!u_probe_offsets(somain, g_solist_head, vdso, hp)) {
    SLOGE("solist-unload: offset probe failed [size=%zu next=%zu ctor=%zu]",
          g_size_off, g_next_off, g_ctor_off);
    return false;
  }

  SLOGI("solist-unload: ready [size=%zu next=%zu ctor=%zu] unload=%p",
        g_size_off, g_next_off, g_ctor_off,
        reinterpret_cast<void *>(g_soinfo_unload));
  g_unload_ok = true;
  return true;
}

} // namespace

int hide_from_solist(const char *path_substr) {
  if (!u_init() || g_solist_head_slot == nullptr) {
    SLOGE("solist: linker state unavailable; skip hiding");
    return 0;
  }

  void *head = *g_solist_head_slot;
  if (head == nullptr)
    return 0;

  /* Sanity-check the solist head. */
  const char *head_path = g_realpath_u(head);
  if (head_path == nullptr || strstr(head_path, "linker") == nullptr) {
    SLOGE("solist: head realpath '%s' not linker-like; skip hiding",
          head_path != nullptr ? head_path : "(null)");
    return 0;
  }

  int hidden = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  g_pdg_ctor_u(guard_obj); /* unlock the protected linker data once */

  void *prev = nullptr;
  void *cur = head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = u_next(cur);
    const char *p = g_realpath_u(cur);
    if (p != nullptr && strstr(p, path_substr) != nullptr) {
      SLOGI("solist: unlinking %s", p);
      if (prev == nullptr)
        *g_solist_head_slot = next;
      else
        u_set_next(prev, next);
      ++hidden;
      /* prev stays; cur removed */
    } else {
      prev = cur;
    }
    cur = next;
  }

  g_pdg_dtor_u(guard_obj); /* re-lock */
  g_solist_head = *g_solist_head_slot;

  SLOGI("solist: hid %d entry(ies) matching '%s'", hidden, path_substr);
  return hidden;
}

bool prepare_linker() { return u_init(); }

int drop_module_from_solist(const char *path_substr, bool dry_run,
                            bool keep_mapped) {
  if (!u_init())
    return 0;

  int n = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  g_pdg_ctor_u(guard_obj);
  void *cur = g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = u_next(cur); /* save before soinfo_unload mutates the list */
    const char *p = g_realpath_u(cur);
    const char *sn = g_soname_u != nullptr ? g_soname_u(cur) : nullptr;
    /* Match realpath or soname. */
    bool match = (p != nullptr && strstr(p, path_substr) != nullptr) ||
                 (sn != nullptr && strstr(sn, path_substr) != nullptr);
    if (!keep_mapped && !dry_run && p != nullptr &&
        strncmp(p, "/system", 7) != 0 && strncmp(p, "/apex", 5) != 0 &&
        strncmp(p, "/vendor", 7) != 0 && strncmp(p, "/product", 8) != 0 &&
        strncmp(p, "/system_ext", 11) != 0)
      SLOGI("solist-scan: realpath=%s soname=%s size=%zu", p,
            sn != nullptr ? sn : "(null)", u_size(cur));
    if (match && u_size(cur) > 0) {
      if (dry_run) {
        SLOGI("solist-unload[dry]: would drop realpath=%s soname=%s (size=%zu)",
              p != nullptr ? p : "(null)", sn != nullptr ? sn : "(null)",
              u_size(cur));
      } else {
        SLOGI("solist-unload: dropping realpath=%s soname=%s (munmap=%d)",
              p != nullptr ? p : "(null)", sn != nullptr ? sn : "(null)",
              !keep_mapped);
        if (keep_mapped)
          u_set_size(cur, 0);   /* skip munmap -> module code survives */
        u_set_ctor(cur, false); /* don't run its DT_FINI */
        g_soinfo_unload(cur);
        u_set_ctor(cur, true);
        if (g_load_counter != nullptr && g_unload_counter != nullptr &&
            *g_load_counter > 0 && *g_unload_counter > 0) {
          --(*g_load_counter);
          --(*g_unload_counter);
        }
      }
      ++n;
    }
    cur = next;
  }
  g_pdg_dtor_u(guard_obj);
  SLOGI("solist-unload: %s %d module seg(s) matching '%s'",
        dry_run ? "[dry] found" : "dropped", n, path_substr);
  return n;
}

int drop_lib_containing(uintptr_t addr, bool keep_mapped) {
  if (!u_init() || addr == 0)
    return 0;
  if (g_size_off < sizeof(void *))
    return 0;
  const size_t base_off = g_size_off - sizeof(void *);

  int n = 0;
  char guard_obj[16] = {}; /* dummy `this`; guard touches only linker globals */
  g_pdg_ctor_u(guard_obj);
  void *cur = g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    void *next = u_next(cur); /* save before soinfo_unload mutates the list */
    uintptr_t base = *reinterpret_cast<uintptr_t *>(
        reinterpret_cast<uintptr_t>(cur) + base_off);
    size_t size = u_size(cur);
    if (size > 0 && base != 0 && addr >= base && addr < base + size) {
      const char *p = g_realpath_u(cur);
      SLOGI("solist-unload: dropping loader realpath=%s base=%p size=%zu "
            "munmap=%d",
            p != nullptr ? p : "(null)", reinterpret_cast<void *>(base), size,
            !keep_mapped);
      if (keep_mapped)
        u_set_size(cur, 0);   /* unload skips munmap; mapping survives */
      u_set_ctor(cur, false); /* skip the spent loader's DT_FINI (a no-op) */
      g_soinfo_unload(cur);
      if (g_load_counter != nullptr && g_unload_counter != nullptr &&
          *g_load_counter > 0 && *g_unload_counter > 0) {
        --(*g_load_counter);
        --(*g_unload_counter);
      }
      ++n;
      break; /* exactly one library can contain the address */
    }
    cur = next;
  }
  g_pdg_dtor_u(guard_obj);
  SLOGI("solist-unload: drop_lib_containing(%p) -> %d",
        reinterpret_cast<void *>(addr), n);
  return n;
}

static bool u_contains(void *needle) {
  void *cur =
      g_solist_head_slot != nullptr ? *g_solist_head_slot : g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    if (cur == needle)
      return true;
    cur = u_next(cur);
  }
  return false;
}

int release_lib_containing(uintptr_t addr) {
  if (!u_init() || addr == 0 || g_size_off < sizeof(void *))
    return 0;
  const size_t base_off = g_size_off - sizeof(void *);
  bool released = false;
  char guard_obj[16] = {};
  g_pdg_ctor_u(guard_obj);
  void *cur =
      g_solist_head_slot != nullptr ? *g_solist_head_slot : g_solist_head;
  for (int i = 0; i < kMaxWalk && cur != nullptr; ++i) {
    const uintptr_t base = *reinterpret_cast<uintptr_t *>(
        reinterpret_cast<uintptr_t>(cur) + base_off);
    const size_t size = u_size(cur);
    if (size > 0 && base != 0 && addr >= base && addr - base < size) {
      auto *ctor = reinterpret_cast<bool *>(reinterpret_cast<uintptr_t>(cur) +
                                            g_ctor_off);
      const bool constructors_called = *ctor;
      // Keep executing code mapped while releasing only our owned reference.
      u_set_size(cur, 0);
      *ctor = false;
      g_soinfo_unload(cur);
      if (g_load_counter != nullptr && g_unload_counter != nullptr &&
          *g_load_counter > 0 && *g_unload_counter > 0) {
        --(*g_load_counter);
        --(*g_unload_counter);
      }
      released = !u_contains(cur);
      if (!released) {
        // Other linker indexes may still reference this object.
        u_set_size(cur, size);
        *ctor = constructors_called;
      }
      break;
    }
    cur = u_next(cur);
  }
  if (g_solist_head_slot != nullptr)
    g_solist_head = *g_solist_head_slot;
  g_pdg_dtor_u(guard_obj);
  SLOGI("solist-unload: release retained library released=%d",
        released ? 1 : 0);
  return released ? 1 : 0;
}

struct CfiShadowRange {
  uintptr_t start = 0;
  uintptr_t end = 0;
  int prot = 0;
};

uintptr_t cfi_shadow_offset(uintptr_t addr) {
  return (addr >> kCfiShadowGranularity) << 1;
}

bool find_cfi_shadow(CfiShadowRange *out) {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return false;
  char line[512];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, "[anon:cfi shadow]") == nullptr)
      continue;
    unsigned long start = 0, end = 0;
    char perms[5] = {};
    if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
      continue;
    out->start = start;
    out->end = end;
    out->prot = (perms[0] == 'r' ? PROT_READ : 0) |
                (perms[1] == 'w' ? PROT_WRITE : 0) |
                (perms[2] == 'x' ? PROT_EXEC : 0);
    (void)fclose(fp);
    return true;
  }
  (void)fclose(fp);
  return false;
}

bool sync_cfi_shadow(uintptr_t old_start, uintptr_t new_start, size_t size) {
  if (old_start == new_start || size == 0)
    return true;

  CfiShadowRange shadow;
  if (!find_cfi_shadow(&shadow))
    return true;

  uintptr_t old_end = old_start + size - 1;
  uintptr_t new_end = new_start + size - 1;
  uintptr_t src = shadow.start + cfi_shadow_offset(old_start);
  uintptr_t src_end =
      shadow.start + cfi_shadow_offset(old_end) + kCfiShadowEntrySize;
  uintptr_t dst = shadow.start + cfi_shadow_offset(new_start);
  uintptr_t dst_end =
      shadow.start + cfi_shadow_offset(new_end) + kCfiShadowEntrySize;
  if (src < shadow.start || src_end > shadow.end || dst < shadow.start ||
      dst_end > shadow.end || src_end < src || dst_end < dst) {
    SLOGE("cfi-shadow: range outside shadow map");
    return false;
  }

  size_t pg = page_size();
  uintptr_t prot_start = page_down(dst, pg);
  uintptr_t prot_end = page_up(dst_end, pg);
  bool reprotect = (shadow.prot & PROT_WRITE) == 0;
  if (reprotect &&
      mprotect(reinterpret_cast<void *>(prot_start), prot_end - prot_start,
               shadow.prot | PROT_WRITE) != 0) {
    SLOGE("cfi-shadow: make writable failed");
    return false;
  }
  memmove(reinterpret_cast<void *>(dst), reinterpret_cast<const void *>(src),
          dst_end - dst);
  if (reprotect && mprotect(reinterpret_cast<void *>(prot_start),
                            prot_end - prot_start, shadow.prot) != 0) {
    SLOGE("cfi-shadow: restore protection failed");
    return false;
  }
  return true;
}

struct MapRange {
  uintptr_t start, end;
  int prot;
};

static int anonymize_ranges(const MapRange *ranges, int nr) {
  int done = 0;
  for (int i = 0; i < nr; ++i) {
    size_t size = ranges[i].end - ranges[i].start;
    void *addr = reinterpret_cast<void *>(ranges[i].start);
    // Preflight every permission the replacement will need. In particular,
    // executable mappings fail here on an execmem denial, before the original
    // file-backed VMA is displaced.
    int copy_prot = ranges[i].prot | PROT_READ | PROT_WRITE;
    void *copy =
        mmap(nullptr, size, copy_prot, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (copy == MAP_FAILED)
      continue;
    bool added_read = (ranges[i].prot & PROT_READ) == 0;
    if (added_read && mprotect(addr, size, ranges[i].prot | PROT_READ) != 0) {
      munmap(copy, size);
      continue;
    }
    memcpy(copy, addr, size);
    if (added_read && mprotect(addr, size, ranges[i].prot) != 0) {
      SLOGE("maps-spoof: failed to restore source protection");
      munmap(copy, size);
      continue;
    }
    if (mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, addr) ==
        MAP_FAILED) {
      munmap(copy, size);
      continue;
    }
    if (mprotect(addr, size, ranges[i].prot) != 0) {
      SLOGE("maps-spoof: failed to restore anonymous protection");
      continue;
    }
    if (ranges[i].prot & PROT_EXEC) {
      sync_cfi_shadow(ranges[i].start, ranges[i].start, size);
      __builtin___clear_cache(reinterpret_cast<char *>(addr),
                              reinterpret_cast<char *>(ranges[i].start + size));
    }
    ++done;
  }
  return done;
}

struct LoadedObjectScan {
  uintptr_t address = 0;
  MapRange ranges[64]{};
  int count = 0;
};

void add_loaded_range(LoadedObjectScan *scan, uintptr_t start, uintptr_t end,
                      int prot) {
  if (scan->count >= static_cast<int>(std::size(scan->ranges)) || end <= start)
    return;
  scan->ranges[scan->count++] = {start, end, prot};
}

int collect_loaded_object(dl_phdr_info *info, size_t, void *data) {
  auto *scan = static_cast<LoadedObjectScan *>(data);
  const size_t pg = page_size();
  if (pg == 0)
    return 0;

  bool contains = false;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD)
      continue;
    uintptr_t start = elf_runtime_address(info->dlpi_addr, ph.p_vaddr);
    if (ph.p_memsz > UINTPTR_MAX - start)
      continue;
    if (scan->address >= start && scan->address < start + ph.p_memsz) {
      contains = true;
      break;
    }
  }
  if (!contains)
    return 0;

  uintptr_t relro_start = 0;
  uintptr_t relro_end = 0;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_GNU_RELRO)
      continue;
    uintptr_t start = elf_runtime_address(info->dlpi_addr, ph.p_vaddr);
    if (ph.p_memsz > UINTPTR_MAX - start ||
        start + ph.p_memsz > UINTPTR_MAX - (pg - 1))
      continue;
    relro_start = page_down(start, pg);
    relro_end = page_up(start + ph.p_memsz, pg);
    break;
  }

  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || ph.p_filesz == 0)
      continue;
    uintptr_t file_start = elf_runtime_address(info->dlpi_addr, ph.p_vaddr);
    if (ph.p_filesz > UINTPTR_MAX - file_start ||
        file_start + ph.p_filesz > UINTPTR_MAX - (pg - 1))
      continue;
    uintptr_t start = page_down(file_start, pg);
    uintptr_t end = page_up(file_start + ph.p_filesz, pg);
    int prot = (ph.p_flags & PF_R ? PROT_READ : 0) |
               (ph.p_flags & PF_W ? PROT_WRITE : 0) |
               (ph.p_flags & PF_X ? PROT_EXEC : 0);

    uintptr_t protected_start = std::max(start, relro_start);
    uintptr_t protected_end = std::min(end, relro_end);
    if (relro_end <= relro_start || protected_end <= protected_start) {
      add_loaded_range(scan, start, end, prot);
      continue;
    }
    add_loaded_range(scan, start, protected_start, prot);
    add_loaded_range(scan, protected_start, protected_end, prot & ~PROT_WRITE);
    add_loaded_range(scan, protected_end, end, prot);
  }
  return 1;
}

int normalize_loaded_ranges(MapRange *ranges, int count) {
  std::sort(ranges, ranges + count, [](const MapRange &a, const MapRange &b) {
    return a.start < b.start || (a.start == b.start && a.end < b.end);
  });
  int output = 0;
  for (int i = 0; i < count; ++i) {
    MapRange range = ranges[i];
    if (output > 0 && range.start < ranges[output - 1].end)
      range.start = ranges[output - 1].end;
    if (range.end <= range.start)
      continue;
    if (output > 0 && ranges[output - 1].end == range.start &&
        ranges[output - 1].prot == range.prot) {
      ranges[output - 1].end = range.end;
      continue;
    }
    ranges[output++] = range;
  }
  return output;
}

int spoof_loaded_object_maps(uintptr_t address, bool private_only) {
  if (address == 0)
    return 0;
#if defined(__arm__)
  address &= ~static_cast<uintptr_t>(1);
#endif
  LoadedObjectScan scan;
  scan.address = address;
  if (dl_iterate_phdr(collect_loaded_object, &scan) == 0 || scan.count == 0)
    return 0;
  if (!private_only)
    SLOGI("maps-spoof: loaded objects are private mappings");
  scan.count = normalize_loaded_ranges(scan.ranges, scan.count);
  int done = anonymize_ranges(scan.ranges, scan.count);
  SLOGI("maps-spoof: anonymized %d/%d loaded-object segment(s)", done,
        scan.count);
  return done;
}

/* Maps anonymization. */
int spoof_virtual_maps(const char *path_substr, bool private_only) {
  if (path_substr == nullptr || path_substr[0] == '\0')
    return 0;
  MapRange ranges[64];
  int nr = 0;

  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return 0;
  char line[512];
  while (nr < 64 && fgets(line, sizeof(line), fp) != nullptr) {
    if (strstr(line, path_substr) == nullptr)
      continue;
    unsigned long start = 0, end = 0;
    char perms[5] = {};
    if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
      continue;
    if (private_only && perms[3] != 'p')
      continue; // skip shared mappings (e.g. ART's own memfd) when asked
    int prot = (perms[0] == 'r' ? PROT_READ : 0) |
               (perms[1] == 'w' ? PROT_WRITE : 0) |
               (perms[2] == 'x' ? PROT_EXEC : 0);
    ranges[nr++] = {start, end, prot};
  }
  (void)fclose(fp);

  int done = anonymize_ranges(ranges, nr);
  SLOGI("maps-spoof: anonymized %d/%d segment(s) matching '%s'", done, nr,
        path_substr);
  return done;
}

int spoof_fd_maps(int fd, bool private_only) {
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) != 0)
    return 0;

  MapRange ranges[64];
  int nr = 0;
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return 0;
  char line[512];
  while (nr < 64 && fgets(line, sizeof(line), fp) != nullptr) {
    unsigned long start = 0, end = 0;
    unsigned int dev_major = 0, dev_minor = 0;
    unsigned long inode = 0;
    char perms[5] = {};
    if (sscanf(line, "%lx-%lx %4s %*s %x:%x %lu", &start, &end, perms,
               &dev_major, &dev_minor, &inode) != 6)
      continue;
    if (makedev(dev_major, dev_minor) != st.st_dev ||
        static_cast<ino_t>(inode) != st.st_ino)
      continue;
    if (private_only && perms[3] != 'p')
      continue;
    int prot = (perms[0] == 'r' ? PROT_READ : 0) |
               (perms[1] == 'w' ? PROT_WRITE : 0) |
               (perms[2] == 'x' ? PROT_EXEC : 0);
    ranges[nr++] = {start, end, prot};
  }
  (void)fclose(fp);

  int done = anonymize_ranges(ranges, nr);
  SLOGI("maps-spoof: anonymized %d/%d segment(s) for fd=%d", done, nr, fd);
  return done;
}

int name_anonymous_exec() {
  FILE *fp = fopen("/proc/self/maps", "re");
  if (fp == nullptr)
    return 0;
  char line[512];
  int n = 0;
  while (fgets(line, sizeof(line), fp) != nullptr) {
    unsigned long start = 0, end = 0;
    char perms[5] = {};
    int path_off = 0;
    if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &start, &end, perms,
               &path_off) < 3)
      continue;
    if (strchr(perms, 'x') == nullptr)
      continue; // executable only
    const char *path = line + path_off;
    while (*path == ' ')
      ++path;
    if (*path != '\0' && *path != '\n')
      continue; // already named or file-backed -> leave alone
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<void *>(start),
          end - start, "dalvik-jit-code-cache");
    ++n;
  }
  (void)fclose(fp);
  SLOGI("maps-spoof: named %d bare anon exec seg(s)", n);
  return n;
}

} // namespace yuki::solist
