#include "../../../kernel/feature/yukizygisk/native_stub.h"
#include <android/dlext.h>
#include <asm/unistd.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <yukilinker.h>

_Static_assert(sizeof(yukilinker_c_native_origin) == 296, "native origin ABI");
#define CHECK_OFFSET(field, value)                                             \
  _Static_assert(offsetof(struct yz_native_stub, field) == value,              \
                 "stub " #field)
CHECK_OFFSET(entry, YZ_NS_ENTRY);
CHECK_OFFSET(linker, YZ_NS_LINKER);
CHECK_OFFSET(resolver, YZ_NS_RESOLVER);
CHECK_OFFSET(resolver_size, YZ_NS_LENGTH);
CHECK_OFFSET(bootstrap, YZ_NS_BOOTSTRAP);
CHECK_OFFSET(loader, YZ_NS_LOADER);
CHECK_OFFSET(process, YZ_NS_PID);
CHECK_OFFSET(generation, YZ_NS_GENERATION);
CHECK_OFFSET(loader_fd, YZ_NS_LOADER_FD);
CHECK_OFFSET(core_fd, YZ_NS_CORE_FD);
CHECK_OFFSET(packet_argument, YZ_NS_PACKET);
CHECK_OFFSET(cleanup_count, YZ_NS_COUNT);
CHECK_OFFSET(cleanup_fds, YZ_NS_FDS);
CHECK_OFFSET(status, YZ_NS_STATUS);
CHECK_OFFSET(message, YZ_NS_MESSAGE);
CHECK_OFFSET(report_fd, YZ_NS_REPORT_FD);
CHECK_OFFSET(completion, YZ_NS_COMPLETION);
CHECK_OFFSET(completion.error, YZ_NS_ERROR);
CHECK_OFFSET(completion.ownership, YZ_NS_OWNERSHIP);
CHECK_OFFSET(close_loader, YZ_NS_CLOSE);
#undef CHECK_OFFSET
_Static_assert(sizeof(struct yz_native_stub) == 664, "stub size");
_Static_assert(YZ_NS_MESSAGE + 256 - YZ_NS_STATUS == YZ_NS_REPORT_SIZE,
               "report size");

/* This translation unit is freestanding. Only validated linker functions and
 * direct syscalls may be called before the loader has a Bionic identity. */
static long call4(long number, long a, long b, long c, long d) {
#if defined(__aarch64__)
  register long r0 __asm__("x0") = a, r1 __asm__("x1") = b;
  register long r2 __asm__("x2") = c, r3 __asm__("x3") = d;
  register long nr __asm__("x8") = number;
  __asm__ volatile("svc #0"
                   : "+r"(r0)
                   : "r"(r1), "r"(r2), "r"(r3), "r"(nr)
                   : "memory", "cc");
#else
  register long r0 __asm__("r0") = a, r1 __asm__("r1") = b;
  register long r2 __asm__("r2") = c, r3 __asm__("r3") = d;
  register long nr __asm__("r7") = number;
  __asm__ volatile("svc #0"
                   : "+r"(r0)
                   : "r"(r1), "r"(r2), "r"(r3), "r"(nr)
                   : "memory", "cc");
#endif
  return r0;
}

static void zero(void *p, size_t n) {
  volatile unsigned char *s = p;
  while (n--)
    *s++ = 0;
}
static int equal(const char *a, const char *b, size_t limit) {
  for (size_t i = 0; i < limit; ++i) {
    if (a[i] != b[i])
      return 0;
    if (!a[i])
      return 1;
  }
  return 0;
}
static int copy(char *out, size_t capacity, const char *in, size_t limit) {
  for (size_t i = 0; i < capacity && i < limit; ++i) {
    out[i] = in[i];
    if (!in[i])
      return 1;
  }
  return 0;
}
struct mapping {
  uintptr_t low, high;
  unsigned permissions;
};
struct maps {
  struct mapping items[512];
  unsigned count;
};

static int hex(const char **cursor, uintptr_t *value) {
  const char *p = *cursor;
  uintptr_t n = 0;
  unsigned digits = 0;
  for (;;) {
    unsigned v = *p >= '0' && *p <= '9'   ? (unsigned)(*p - '0')
                 : *p >= 'a' && *p <= 'f' ? (unsigned)(*p - 'a' + 10)
                                          : 16;
    if (v == 16)
      break;
    if (++digits > sizeof(uintptr_t) * 2)
      return 0;
    n = (n << 4) | v;
    ++p;
  }
  *cursor = p;
  *value = n;
  return digits != 0;
}

static int read_maps(struct maps *maps) {
  char buffer[4096];
  size_t used = 0;
  maps->count = 0;
  long fd = call4(__NR_openat, AT_FDCWD, (long)"/proc/self/maps",
                  O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return 0;
  int valid = 1;
  for (;;) {
    long n =
        call4(__NR_read, fd, (long)(buffer + used), sizeof(buffer) - used, 0);
    if (n == -4)
      continue;
    if (n <= 0) {
      valid = n == 0 && used == 0;
      break;
    }
    used += (size_t)n;
    size_t first = 0;
    for (size_t i = 0; i < used; ++i) {
      /* The raw read syscall fills n bytes; the analyzer cannot model svc. */
      // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
      if (buffer[i] != '\n')
        continue;
      buffer[i] = 0;
      const char *p = buffer + first;
      struct mapping m;
      if (!hex(&p, &m.low) || *p++ != '-' || !hex(&p, &m.high) ||
          m.low >= m.high || *p++ != ' ' || i - (size_t)(p - buffer) < 4 ||
          maps->count == 512 ||
          (maps->count && maps->items[maps->count - 1].high > m.low)) {
        valid = 0;
        goto done;
      }
      m.permissions = (p[0] == 'r' ? PF_R : 0) | (p[1] == 'w' ? PF_W : 0) |
                      (p[2] == 'x' ? PF_X : 0);
      maps->items[maps->count++] = m;
      first = i + 1;
    }
    for (size_t i = first; i < used; ++i)
      buffer[i - first] = buffer[i];
    used -= first;
    if (used == sizeof(buffer)) {
      valid = 0;
      break;
    }
  }
done:
  return call4(__NR_close, fd, 0, 0, 0) == 0 && valid && maps->count;
}

static int mapped(const struct maps *maps, uintptr_t address, size_t size,
                  unsigned flags) {
  if (!size || size > UINTPTR_MAX - address)
    return 0;
  const uintptr_t end = address + size;
  for (unsigned i = 0; i < maps->count; ++i) {
    const struct mapping *m = &maps->items[i];
    if (m->high <= address)
      continue;
    if (m->low > address || (m->permissions & flags) != flags)
      return 0;
    if (m->high >= end)
      return 1;
    address = m->high;
  }
  return 0;
}

struct object {
  uintptr_t bias;
  const ElfW(Phdr) * ph;
  unsigned count;
  const ElfW(Sym) * symbols;
  const char *strings;
  size_t string_size, symbol_count;
  const char *soname;
  int needs_libc;
};

static int contains(const struct object *obj, uintptr_t address, size_t bytes,
                    unsigned flags) {
  if (address < obj->bias)
    return 0;
  uintptr_t offset = address - obj->bias;
  for (unsigned i = 0; i < obj->count; ++i) {
    const ElfW(Phdr) *p = obj->ph + i;
    if (p->p_type == PT_LOAD && (p->p_flags & flags) == flags &&
        offset >= p->p_vaddr && offset - p->p_vaddr < p->p_memsz &&
        bytes <= p->p_memsz - (offset - p->p_vaddr))
      return 1;
  }
  return 0;
}

static int object(const struct maps *maps, uintptr_t base, struct object *obj) {
  zero(obj, sizeof(*obj));
  if (base % _Alignof(ElfW(Ehdr)) ||
      !mapped(maps, base, sizeof(ElfW(Ehdr)), PF_R))
    return 0;
  const ElfW(Ehdr) *h = (const void *)base;
#if defined(__aarch64__)
  const unsigned machine = EM_AARCH64, klass = ELFCLASS64;
#else
  const unsigned machine = EM_ARM, klass = ELFCLASS32;
#endif
  /* Post-link optimization can place PHDRs in a later PT_LOAD. The complete
   * table is bounded by the address overflow and readable mapping checks. */
  if (h->e_ident[0] != 0x7f || h->e_ident[1] != 'E' || h->e_ident[2] != 'L' ||
      h->e_ident[3] != 'F' || h->e_ident[EI_CLASS] != klass ||
      h->e_ident[EI_DATA] != ELFDATA2LSB ||
      h->e_ident[EI_VERSION] != EV_CURRENT || h->e_machine != machine ||
      h->e_version != EV_CURRENT ||
      (h->e_type != ET_DYN && h->e_type != ET_EXEC) ||
      h->e_ehsize != sizeof(*h) || h->e_phentsize != sizeof(ElfW(Phdr)) ||
      !h->e_phnum || h->e_phnum > 128 || h->e_phoff > UINTPTR_MAX - base ||
      h->e_phoff % _Alignof(ElfW(Phdr)) ||
      !mapped(maps, base + h->e_phoff, h->e_phnum * sizeof(ElfW(Phdr)), PF_R))
    return 0;
  obj->ph = (const void *)(base + h->e_phoff);
  obj->count = h->e_phnum;
  int header = 0;
  for (unsigned i = 0; i < obj->count; ++i) {
    const ElfW(Phdr) *p = obj->ph + i;
    if (p->p_type == PT_LOAD && !p->p_offset && p->p_filesz >= sizeof(*h)) {
      if (header++ || p->p_vaddr > base)
        return 0;
      obj->bias = base - p->p_vaddr;
    }
  }
  if (!header)
    return 0;
  const ElfW(Dyn) *dynamic = 0;
  size_t entries = 0;
  for (unsigned i = 0; i < obj->count; ++i) {
    const ElfW(Phdr) *p = obj->ph + i;
    if (p->p_filesz > p->p_memsz || p->p_vaddr > UINTPTR_MAX - obj->bias ||
        p->p_memsz > UINTPTR_MAX - obj->bias - p->p_vaddr)
      return 0;
    if (p->p_type == PT_DYNAMIC) {
      if (dynamic || !p->p_memsz || p->p_memsz % sizeof(*dynamic))
        return 0;
      dynamic = (const void *)(obj->bias + p->p_vaddr);
      entries = p->p_memsz / sizeof(*dynamic);
    }
  }
  if (!dynamic ||
      !contains(obj, (uintptr_t)dynamic, entries * sizeof(*dynamic), PF_R) ||
      !mapped(maps, (uintptr_t)dynamic, entries * sizeof(*dynamic), PF_R))
    return 0;
  uintptr_t hash = 0, gnu = 0;
  size_t soname = SIZE_MAX;
  int ended = 0, syment = 0;
  for (size_t i = 0; i < entries; ++i) {
    const ElfW(Dyn) *d = dynamic + i;
    if (d->d_tag == DT_NULL) {
      ended = 1;
      break;
    }
    if (d->d_tag == DT_SYMENT)
      syment = d->d_un.d_val == sizeof(ElfW(Sym));
    if (d->d_tag == DT_STRSZ)
      obj->string_size = d->d_un.d_val;
    if (d->d_tag == DT_SONAME)
      soname = d->d_un.d_val;
    if (d->d_tag == DT_STRTAB || d->d_tag == DT_SYMTAB || d->d_tag == DT_HASH ||
        d->d_tag == DT_GNU_HASH) {
      if (d->d_un.d_ptr > UINTPTR_MAX - obj->bias)
        return 0;
      uintptr_t value = obj->bias + d->d_un.d_ptr;
      if (d->d_tag == DT_STRTAB) {
        if (obj->strings)
          return 0;
        obj->strings = (const void *)value;
      }
      if (d->d_tag == DT_SYMTAB) {
        if (obj->symbols)
          return 0;
        obj->symbols = (const void *)value;
      }
      if (d->d_tag == DT_HASH) {
        if (hash)
          return 0;
        hash = value;
      }
      if (d->d_tag == DT_GNU_HASH) {
        if (gnu)
          return 0;
        gnu = value;
      }
    }
  }
  if (!ended || !syment || !obj->symbols || !obj->strings ||
      !obj->string_size ||
      !contains(obj, (uintptr_t)obj->strings, obj->string_size, PF_R) ||
      !mapped(maps, (uintptr_t)obj->strings, obj->string_size, PF_R))
    return 0;
  if (soname != SIZE_MAX) {
    if (soname >= obj->string_size)
      return 0;
    obj->soname = obj->strings + soname;
    size_t i = soname;
    while (i < obj->string_size && obj->strings[i])
      ++i;
    if (i == obj->string_size)
      return 0;
  }
  for (size_t i = 0; i < entries && dynamic[i].d_tag != DT_NULL; ++i) {
    if (dynamic[i].d_tag == DT_NEEDED &&
        dynamic[i].d_un.d_val < obj->string_size &&
        equal(obj->strings + dynamic[i].d_un.d_val, "libc.so",
              obj->string_size - dynamic[i].d_un.d_val))
      obj->needs_libc = 1;
  }
  if (hash) {
    if (hash % 4 || !contains(obj, hash, 8, PF_R) ||
        !mapped(maps, hash, 8, PF_R))
      return 0;
    obj->symbol_count = ((const uint32_t *)hash)[1];
  } else if (gnu) {
    if (gnu % 4 || !contains(obj, gnu, 16, PF_R) ||
        !mapped(maps, gnu, 16, PF_R))
      return 0;
    const uint32_t *g = (const void *)gnu;
    if (!g[0] || g[0] > 65536 || !g[2] || g[2] > 65536 || g[1] > 65536)
      return 0;
    const size_t prefix = 16 + (size_t)g[2] * sizeof(uintptr_t);
    if (prefix > UINTPTR_MAX - gnu || g[0] * 4 > UINTPTR_MAX - gnu - prefix ||
        !contains(obj, gnu, prefix + g[0] * 4, PF_R) ||
        !mapped(maps, gnu, prefix + g[0] * 4, PF_R))
      return 0;
    const uint32_t *buckets = (const void *)(gnu + prefix);
    uint32_t last = 0;
    for (uint32_t i = 0; i < g[0]; ++i)
      if (buckets[i] > last)
        last = buckets[i];
    obj->symbol_count = g[1];
    if (last) {
      if (last < g[1] || last >= 65536)
        return 0;
      uintptr_t chain = (uintptr_t)(buckets + g[0]) + (last - g[1]) * 4;
      for (;;) {
        if (last >= 65536 || !contains(obj, chain, 4, PF_R) ||
            !mapped(maps, chain, 4, PF_R))
          return 0;
        ++last;
        if (*(const uint32_t *)chain & 1)
          break;
        chain += 4;
      }
      obj->symbol_count = last;
    }
  } else
    return 0;
  return obj->symbol_count && obj->symbol_count <= 65536 &&
         (uintptr_t)obj->symbols % _Alignof(ElfW(Sym)) == 0 &&
         contains(obj, (uintptr_t)obj->symbols,
                  obj->symbol_count * sizeof(ElfW(Sym)), PF_R) &&
         mapped(maps, (uintptr_t)obj->symbols,
                obj->symbol_count * sizeof(ElfW(Sym)), PF_R);
}

static void *symbol(const struct maps *maps, const struct object *obj,
                    const char *name) {
  void *result = 0;
  for (size_t i = 0; i < obj->symbol_count; ++i) {
    const ElfW(Sym) *s = obj->symbols + i;
    if (s->st_name >= obj->string_size || s->st_shndx == SHN_UNDEF ||
        !s->st_value ||
        !equal(obj->strings + s->st_name, name, obj->string_size - s->st_name))
      continue;
    if (result || ELF64_ST_TYPE(s->st_info) != STT_FUNC ||
        (ELF64_ST_BIND(s->st_info) != STB_GLOBAL &&
         ELF64_ST_BIND(s->st_info) != STB_WEAK) ||
        s->st_value > UINTPTR_MAX - obj->bias)
      return 0;
    uintptr_t address = obj->bias + s->st_value;
#if defined(__arm__)
    uintptr_t pc = address & ~(uintptr_t)1;
#else
    uintptr_t pc = address;
#endif
    if (!contains(obj, pc, s->st_size ? s->st_size : 1, PF_X) ||
        !mapped(maps, pc, s->st_size ? s->st_size : 1, PF_X))
      return 0;
    result = (void *)address;
  }
  return result;
}

typedef void *(*open_fn)(const char *, int, const android_dlextinfo *,
                         const void *);
typedef void *(*lookup_fn)(void *, const char *, const void *);
typedef int (*address_fn)(const void *, Dl_info *);
struct search {
  struct maps maps;
  struct yz_native_stub *stub;
  open_fn open;
  lookup_fn lookup;
  address_fn address;
  int (*close)(void *);
  int (*iterate)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
  char *(*error)(void);
  unsigned candidates, loaders;
  int invalid;
  void *libc, *anchor;
  uintptr_t base;
  char path[256];
};

static const char *loader_name(void) {
  return sizeof(void *) == 8 ? "libyloader64.so" : "libyloader32.so";
}

static int find_libc(struct dl_phdr_info *info, size_t size, void *opaque) {
  struct search *s = opaque;
  if (size < offsetof(struct dl_phdr_info, dlpi_phnum) +
                 sizeof(info->dlpi_phnum) ||
      !info->dlpi_phnum || info->dlpi_phnum > 128 ||
      !mapped(&s->maps, (uintptr_t)info->dlpi_phdr,
              info->dlpi_phnum * sizeof(ElfW(Phdr)), PF_R)) {
    s->stub->stage = 101;
    s->invalid = 1;
    return 1;
  }
  uintptr_t base = 0;
  for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) *p = info->dlpi_phdr + i;
    /* mapped() validated this complete PHDR range against the live maps. */
    // NOLINTNEXTLINE(clang-analyzer-nullability.NullableDereferenced)
    if (p->p_type == PT_LOAD && p->p_offset == 0 &&
        p->p_filesz >= sizeof(ElfW(Ehdr))) {
      if (base || p->p_vaddr > UINTPTR_MAX - info->dlpi_addr) {
        s->invalid = 1;
        return 1;
      }
      base = info->dlpi_addr + p->p_vaddr;
    }
  }
  struct object obj;
  if (!base || !object(&s->maps, base, &obj) || obj.bias != info->dlpi_addr ||
      obj.ph != info->dlpi_phdr || obj.count != info->dlpi_phnum) {
    s->stub->stage = 102;
    s->invalid = 1;
    return 1;
  }
  if (!obj.soname)
    return 0;
  if (equal(obj.soname, loader_name(), 32))
    ++s->loaders;
  if (!equal(obj.soname, "libc.so", 8))
    return 0;
  if (++s->candidates != 1)
    return 0;
  s->anchor = symbol(&s->maps, &obj, "getpid");
  Dl_info identity;
  zero(&identity, sizeof(identity));
  if (!s->anchor || !s->address(s->anchor, &identity) ||
      (uintptr_t)identity.dli_fbase != base || !identity.dli_fname) {
    s->stub->stage = 103;
    s->invalid = 1;
    return 1;
  }
  for (size_t i = 0; i < sizeof(s->path); ++i) {
    if (!mapped(&s->maps, (uintptr_t)identity.dli_fname + i, 1, PF_R)) {
      s->stub->stage = 104;
      s->invalid = 1;
      return 1;
    }
    s->path[i] = identity.dli_fname[i];
    if (!s->path[i])
      break;
  }
  if (s->path[0] != '/' || s->path[255]) {
    s->stub->stage = 105;
    s->invalid = 1;
    return 1;
  }
  s->base = base;
  /* Enumeration holds Bionic's recursive linker lock. Pin the exact object
   * before leaving the callback; NOLOAD cannot execute new constructors. */
  s->libc = s->open("libc.so", RTLD_NOW | RTLD_NOLOAD, 0, s->anchor);
  s->stub->libc_reference = (uintptr_t)s->libc;
  if (!s->libc || s->lookup(s->libc, "getpid", s->anchor) != s->anchor) {
    s->stub->stage = 106;
    s->invalid = 1;
    const char *error = s->error();
    copy(s->stub->message, sizeof(s->stub->message),
         error ? error : "libc pin identity mismatch", 256);
  }
  return s->invalid;
}

static void failure(struct yz_native_stub *stub, int code, const char *text) {
  stub->status = code;
  stub->bootstrap = 0;
  copy(stub->message, sizeof(stub->message), text, sizeof(stub->message));
  size_t n = 0;
  while (n < sizeof(stub->message) && stub->message[n])
    ++n;
  call4(__NR_write, 2, (long)stub->message, n, 0);
}

static int release_libc(struct search *s) {
  if (!s->libc)
    return 1;
  if (s->close(s->libc) != 0) {
    failure(
        s->stub, 7,
        "native bootstrap: libc pin release failed; retaining references\n");
    return 0;
  }
  s->libc = 0;
  s->stub->libc_reference = 0;
  return 1;
}

static int load_locked(struct dl_phdr_info *, size_t, void *);

__attribute__((section(".text.entry"), visibility("hidden"))) void
yz_native_resolve(struct yz_native_stub *stub) {
  if (stub->magic != YZ_NATIVE_STUB_MAGIC ||
      stub->version != YZ_NATIVE_STUB_VERSION ||
      stub->process != (uint32_t)call4(__NR_getpid, 0, 0, 0, 0) ||
      !stub->generation || stub->cleanup_count > 68 || stub->loader_fd < 0) {
    if (stub->cleanup_count > 68)
      stub->cleanup_count = 0;
    failure(stub, 1, "native bootstrap: invalid descriptor\n");
    return;
  }
  struct search search;
  zero(&search, sizeof(search));
  search.stub = stub;
  struct object linker;
  if (!read_maps(&search.maps) ||
      !object(&search.maps, stub->linker, &linker)) {
    failure(stub, 2, "native bootstrap: invalid linker ELF/maps\n");
    return;
  }
  search.open =
      (open_fn)symbol(&search.maps, &linker, "__loader_android_dlopen_ext");
  search.lookup = (lookup_fn)symbol(&search.maps, &linker, "__loader_dlsym");
  search.address = (address_fn)symbol(&search.maps, &linker, "__loader_dladdr");
  search.close =
      (int (*)(void *))symbol(&search.maps, &linker, "__loader_dlclose");
  search.iterate =
      (void *)symbol(&search.maps, &linker, "__loader_dl_iterate_phdr");
  search.error = (void *)symbol(&search.maps, &linker, "__loader_dlerror");
  if (!search.open || !search.lookup || !search.address || !search.close ||
      !search.iterate || !search.error) {
    failure(stub, 3, "native bootstrap: unsupported Bionic entry ABI\n");
    return;
  }
  /* Keep the recursive Bionic lock from the outer enumeration until binding
   * completes. Another thread cannot insert a same-SONAME loader between the
   * complete inner census and the FORCE_LOAD operation. */
  search.iterate(load_locked, &search);
}

static int load_locked(struct dl_phdr_info *unused, size_t size, void *opaque) {
  (void)unused;
  (void)size;
  struct search *s = opaque;
  struct yz_native_stub *stub = s->stub;
  if (!read_maps(&s->maps)) {
    failure(stub, 2, "native bootstrap: maps changed during census\n");
    return 1;
  }
  s->iterate(find_libc, s);
  if (s->invalid || s->candidates != 1 || !s->libc || s->loaders) {
    if (!release_libc(s))
      return 1;
    failure(stub, s->loaders ? 5 : 4,
            stub->message[0] ? stub->message
            : s->loaders
                ? "native bootstrap: loader already exists\n"
                : "native bootstrap: libc missing, invalid or ambiguous\n");
    return 1;
  }
  android_dlextinfo ext;
  zero(&ext, sizeof(ext));
  ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_FORCE_LOAD;
  ext.library_fd = stub->loader_fd;
  s->error();
  void *loader = s->open(loader_name(), RTLD_NOW | RTLD_LOCAL, &ext, s->anchor);
  stub->loader = (uintptr_t)loader;
  if (!loader) {
    const char *message = s->error();
    failure(stub, 6,
            message ? message : "native bootstrap: Bionic open failed\n");
    release_libc(s);
    return 1;
  }
  void *entry = s->lookup(loader, "yuki_bootstrap_native_v1", s->anchor);
  bool (*bind)(const yukilinker_c_native_binding *) =
      (void *)s->lookup(loader, "yuki_bootstrap_bind_native_v1", s->anchor);
  Dl_info own, binding;
  struct object loaded;
  zero(&own, sizeof(own));
  zero(&binding, sizeof(binding));
  int valid = entry && bind && s->address(entry, &own) &&
              s->address((void *)bind, &binding) &&
              own.dli_fbase == binding.dli_fbase && read_maps(&s->maps) &&
              object(&s->maps, (uintptr_t)own.dli_fbase, &loaded) &&
              loaded.soname && equal(loaded.soname, loader_name(), 32) &&
              loaded.needs_libc &&
              s->lookup(loader, "getpid", s->anchor) == s->anchor;
  /* DT_NEEDED and the concrete-handle lookup establish the loader's independent
   * libc reference before the temporary pin is dropped. */
  if (!release_libc(s)) {
    return 1;
  }
  if (!valid) {
    if (s->close(loader) == 0)
      stub->loader = 0;
    failure(stub, 8,
            "native bootstrap: native bind/entry capability mismatch\n");
    return 1;
  }
  yukilinker_c_native_binding request;
  zero(&request, sizeof(request));
  request.size = sizeof(request);
  request.version = YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION;
  request.loader_reference = loader;
  _Static_assert(offsetof(struct yz_native_stub, completion) ==
                     YZ_NS_COMPLETION,
                 "native completion ABI");
  _Static_assert(sizeof(stub->completion) ==
                     sizeof(yukilinker_c_native_completion),
                 "native completion layout");
  stub->completion.size = sizeof(stub->completion);
  stub->completion.version = YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION;
  stub->completion.process = stub->process;
  stub->completion.generation = stub->generation;
  request.completion = (yukilinker_c_native_completion *)&stub->completion;
  stub->close_loader = (uintptr_t)s->close;
  request.origin.size = sizeof(request.origin);
  request.origin.version = YUKILINKER_C_NATIVE_BOOTSTRAP_VERSION;
  request.origin.process = stub->process;
  request.origin.generation = stub->generation;
  request.origin.linker_base = stub->linker;
  request.origin.libc_base = s->base;
  request.origin.libc_anchor = (uintptr_t)s->anchor;
  copy(request.origin.libc_path, sizeof(request.origin.libc_path), s->path,
       sizeof(s->path));
  if (!bind(&request)) {
    if (s->close(loader) == 0)
      stub->loader = 0;
    failure(stub, 9, "native bootstrap: bind refused\n");
    return 1;
  }
  stub->status = 0;
  stub->bootstrap = (uintptr_t)entry;
  return 1;
}
