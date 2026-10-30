#include "core_bindings.hpp"
#include "loader_handoff.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <exception>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Key {
  pthread_key_t value{};
  void (*destroy)(void *) = nullptr;
  uint64_t generation = 0;
  bool live = false;
};
Key keys[PTHREAD_KEYS_MAX];
pthread_mutex_t key_mutex = PTHREAD_MUTEX_INITIALIZER;
uint64_t next_generation = 1;

#if !defined(YUKILINKER_BOOTSTRAP)
extern "C" __attribute__((visibility("hidden"))) const ElfW(Ehdr) __ehdr_start;

bool own_headers(dl_phdr_info &info) noexcept {
  const auto &header = __ehdr_start;
  if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_phentsize != sizeof(ElfW(Phdr)) || header.e_phnum == 0 ||
      header.e_phoff > SIZE_MAX - header.e_phnum * sizeof(ElfW(Phdr)))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(&header);
  if (header.e_phoff > UINTPTR_MAX - base)
    return false;
  const auto *headers =
      reinterpret_cast<const ElfW(Phdr) *>(base + header.e_phoff);
  for (size_t i = 0; i < header.e_phnum; ++i) {
    const auto &ph = headers[i];
    if (ph.p_type != PT_LOAD || ph.p_offset != 0 || !(ph.p_flags & PF_R))
      continue;
    if (ph.p_vaddr > base ||
        ph.p_filesz < header.e_phoff + header.e_phnum * sizeof(ElfW(Phdr)))
      return false;
    info.dlpi_addr = base - ph.p_vaddr;
    info.dlpi_name = "";
    info.dlpi_phdr = headers;
    info.dlpi_phnum = header.e_phnum;
    return true;
  }
  return false;
}
#endif
} // namespace

extern "C" int __real_pthread_key_create(pthread_key_t *, void (*)(void *));
extern "C" int __real_pthread_key_delete(pthread_key_t);

extern "C" __attribute__((visibility("hidden"))) int
__wrap_pthread_key_create(pthread_key_t *key, void (*destroy)(void *)) {
  const int locked = pthread_mutex_lock(&key_mutex);
  if (locked != 0)
    return locked;
  Key *free = nullptr;
  for (auto &entry : keys)
    if (!entry.live) {
      free = &entry;
      break;
    }
  int result = EAGAIN;
  if (free != nullptr && next_generation != 0) {
    result = __real_pthread_key_create(key, destroy);
    if (result == 0)
      *free = {*key, destroy, next_generation++, true};
  }
  pthread_mutex_unlock(&key_mutex);
  return result;
}

extern "C" __attribute__((visibility("hidden"))) int
__wrap_pthread_key_delete(pthread_key_t key) {
  const int locked = pthread_mutex_lock(&key_mutex);
  if (locked != 0)
    return locked;
  const int result = __real_pthread_key_delete(key);
  if (result == 0)
    for (auto &entry : keys)
      if (entry.live && entry.value == key) {
        entry.live = false;
        break;
      }
  pthread_mutex_unlock(&key_mutex);
  return result;
}

bool core_private_tsd_used() noexcept {
  if (pthread_mutex_lock(&key_mutex) != 0)
    return true;
  bool live = false;
  for (const auto &entry : keys)
    live = live || entry.live;
  pthread_mutex_unlock(&key_mutex);
  return live;
}

int core_drain_current_thread_tsd() noexcept {
  // The caller has excluded other participants, including destructor epilogues.
  for (int round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; ++round) {
    const int locked = pthread_mutex_lock(&key_mutex);
    if (locked != 0)
      return locked;
    const uint64_t ceiling = next_generation - 1;
    pthread_mutex_unlock(&key_mutex);
    for (const Key &slot : keys) {
      int result = pthread_mutex_lock(&key_mutex);
      if (result != 0)
        return result;
      const Key entry = slot;
      void *value = entry.live && entry.generation <= ceiling
                        ? pthread_getspecific(entry.value)
                        : nullptr;
      if (value != nullptr && entry.destroy == nullptr) {
        pthread_mutex_unlock(&key_mutex);
        return EBUSY;
      }
      if (value != nullptr)
        result = pthread_setspecific(entry.value, nullptr);
      pthread_mutex_unlock(&key_mutex);
      if (result != 0)
        return result;
      if (value != nullptr)
        entry.destroy(value);
      // Never consume the saved registration after a callback; it may be
      // replaced.
    }
  }
  const int locked = pthread_mutex_lock(&key_mutex);
  if (locked != 0)
    return locked;
  int result = 0;
  for (const auto &entry : keys)
    if (entry.live && pthread_getspecific(entry.value) != nullptr)
      result = EBUSY;
  pthread_mutex_unlock(&key_mutex);
  return result;
}

#if !defined(YUKILINKER_BOOTSTRAP)
namespace {
struct RuntimeRetirement {
  uintptr_t base = 0;
  size_t span = 0;
  void (*early)() = nullptr;
  void (*late)() = nullptr;
  CoreUnmapFunction unmap = nullptr;
  bool early_done = false;
  bool closed = false;
  bool busy = false;
  Key final_key{};
};
RuntimeRetirement retirement;

bool own_range(const dl_phdr_info &info, uintptr_t address, size_t bytes,
               unsigned flags) noexcept {
  for (size_t i = 0; i < info.dlpi_phnum; ++i) {
    const auto &ph = info.dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & flags) != flags ||
        ph.p_vaddr > UINTPTR_MAX - info.dlpi_addr)
      continue;
    const uintptr_t begin = info.dlpi_addr + ph.p_vaddr;
    if (address >= begin && address - begin <= ph.p_memsz &&
        bytes <= ph.p_memsz - (address - begin))
      return true;
  }
  return false;
}

int runtime_thread_state(Key *remaining = nullptr) noexcept {
  const int locked = pthread_mutex_lock(&key_mutex);
  if (locked != 0)
    return -locked;
  if (remaining != nullptr)
    *remaining = {};
  size_t count = 0;
  bool has_value = false;
  for (const auto &entry : keys) {
    if (!entry.live)
      continue;
    ++count;
    has_value |= pthread_getspecific(entry.value) != nullptr;
    if (remaining != nullptr)
      *remaining = entry;
  }
  pthread_mutex_unlock(&key_mutex);
  if (count > 1)
    return -EBUSY;
  return has_value ? 1 : 0;
}

int finish_closed_key() noexcept {
  const int locked = pthread_mutex_lock(&key_mutex);
  if (locked != 0)
    return locked;
  int error = 0;
  for (auto &entry : keys) {
    if (!entry.live)
      continue;
    if (!retirement.final_key.live ||
        entry.value != retirement.final_key.value ||
        entry.generation != retirement.final_key.generation ||
        pthread_getspecific(entry.value) != nullptr) {
      error = EBUSY;
      break;
    }
    error = __real_pthread_key_delete(entry.value);
    if (error != 0)
      break;
    entry.live = false;
  }
  pthread_mutex_unlock(&key_mutex);
  return error;
}
} // namespace

bool core_prepare_runtime_retirement(uintptr_t base, size_t span) noexcept {
  if (retirement.base != 0)
    return retirement.base == base && retirement.span == span;
  dl_phdr_info info{};
  const long page = sysconf(_SC_PAGESIZE);
  if (!own_headers(info) || info.dlpi_addr != base || page <= 0 || span == 0 ||
      base % static_cast<uintptr_t>(page) != 0 ||
      span % static_cast<size_t>(page) != 0 || span > UINTPTR_MAX - base)
    return false;
  const ElfW(Dyn) *dynamic = nullptr;
  size_t dynamic_count = 0;
  for (size_t i = 0; i < info.dlpi_phnum; ++i) {
    const auto &ph = info.dlpi_phdr[i];
    if ((ph.p_type == PT_TLS && ph.p_memsz != 0) || ph.p_vaddr > span ||
        ph.p_memsz > span - ph.p_vaddr)
      return false;
    if (ph.p_type == PT_DYNAMIC) {
      if (dynamic != nullptr || ph.p_memsz % sizeof(ElfW(Dyn)) != 0 ||
          !own_range(info, base + ph.p_vaddr, ph.p_memsz, PF_R))
        return false;
      dynamic = reinterpret_cast<const ElfW(Dyn) *>(base + ph.p_vaddr);
      dynamic_count = ph.p_memsz / sizeof(ElfW(Dyn));
    }
  }
  uintptr_t array = 0;
  uintptr_t strings = 0;
  size_t array_bytes = 0;
  size_t string_bytes = 0;
  size_t needed_count = 0;
  size_t needed[3]{};
  bool have_array = false;
  bool have_size = false;
  bool have_strings = false;
  bool have_string_size = false;
  bool terminated = false;
  for (size_t i = 0; i < dynamic_count; ++i) {
    const auto &entry = dynamic[i];
    if (entry.d_tag == DT_NULL) {
      terminated = true;
      break;
    }
    // ELF tags select the active member of the ABI-defined union.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
    switch (entry.d_tag) {
    case DT_FINI:
      if (entry.d_un.d_ptr != 0)
        return false;
      break;
    case DT_FINI_ARRAY:
      if (have_array || entry.d_un.d_ptr > UINTPTR_MAX - base)
        return false;
      have_array = true;
      array = base + entry.d_un.d_ptr;
      break;
    case DT_FINI_ARRAYSZ:
      if (have_size)
        return false;
      have_size = true;
      array_bytes = entry.d_un.d_val;
      break;
    case DT_STRTAB:
      if (have_strings || entry.d_un.d_ptr > UINTPTR_MAX - base)
        return false;
      have_strings = true;
      strings = base + entry.d_un.d_ptr;
      break;
    case DT_STRSZ:
      if (have_string_size)
        return false;
      have_string_size = true;
      string_bytes = entry.d_un.d_val;
      break;
    case DT_NEEDED:
      if (needed_count == 3)
        return false;
      needed[needed_count++] = entry.d_un.d_val;
      break;
    default:
      break;
    }
  }
  // NOLINTEND(cppcoreguidelines-pro-type-union-access)
  if (!terminated || !have_array || !have_size ||
      array_bytes != 2 * sizeof(uintptr_t) ||
      !own_range(info, array, array_bytes, PF_R) || !have_strings ||
      !have_string_size || !own_range(info, strings, string_bytes, PF_R))
    return false;
  for (size_t i = 0; i < needed_count; ++i) {
    if (needed[i] >= string_bytes)
      return false;
    const auto *name = reinterpret_cast<const char *>(strings + needed[i]);
    if (memchr(name, 0, string_bytes - needed[i]) == nullptr ||
        (strcmp(name, "libc.so") != 0 && strcmp(name, "libdl.so") != 0 &&
         strcmp(name, "libm.so") != 0))
      return false;
  }
  uintptr_t fini[2]{};
  memcpy(fini, reinterpret_cast<void *>(array), sizeof(fini));
  for (uintptr_t address : fini)
    if (!own_range(info, address & ~uintptr_t{1}, 1, PF_X))
      return false;
  if (fini[0] == fini[1])
    return false;
  // Anonymous callers have no bionic local group for RTLD_DEFAULT lookup.
  // Use the import already resolved against this image's dependencies.
  CoreUnmapFunction unmap = munmap;
  Dl_info provider{};
  if (unmap == nullptr ||
      dladdr(reinterpret_cast<void *>(unmap), &provider) == 0 ||
      provider.dli_fname == nullptr ||
      own_range(info, reinterpret_cast<uintptr_t>(unmap), 1, PF_X))
    return false;
  const char *name = strrchr(provider.dli_fname, '/');
  name = name == nullptr ? provider.dli_fname : name + 1;
  if (strcmp(name, "libc.so") != 0)
    return false;
  // The final ELF check binds these two entries to the NDK's two CRT stubs.
  retirement = {base,
                span,
                reinterpret_cast<void (*)()>(fini[1]),
                reinterpret_cast<void (*)()>(fini[0]),
                unmap,
                false,
                false,
                false};
  return true;
}

bool core_runtime_profile_ready(uintptr_t base, size_t span) noexcept {
  return retirement.base != 0 && retirement.base == base &&
         retirement.span == span;
}
CoreUnmapFunction core_retirement_unmap() noexcept { return retirement.unmap; }
bool core_runtime_closed() noexcept { return retirement.closed; }

int core_finish_runtime_retirement() noexcept {
  if (retirement.base == 0)
    return EINVAL;
  if (retirement.busy)
    return EBUSY;
  if (retirement.closed)
    return finish_closed_key();
  const int thread_state = runtime_thread_state();
  if (thread_state < 0)
    return -thread_state;
  // With this fixed runtime profile, the loader key has already been deleted.
  // No current-thread value means there is no exception state to query.
  if (thread_state != 0 &&
      (std::uncaught_exceptions() != 0 || std::current_exception()))
    return EBUSY;
  retirement.busy = true;
  if (!retirement.early_done) {
    retirement.early_done = true;
    retirement.early();
  }
  const int drained = core_drain_current_thread_tsd();
  const int remaining = runtime_thread_state(&retirement.final_key);
  if (drained != 0 || remaining != 0) {
    retirement.busy = false;
    if (drained != 0)
      return drained;
    return remaining < 0 ? -remaining : EBUSY;
  }
  retirement.closed = true;
  retirement.late();
  retirement.busy = false;
  return core_private_tsd_used() ? EBUSY : 0;
}

extern "C" __attribute__((visibility("default"))) bool
zygisk_adopt_anonymous_image(uintptr_t base, size_t span, void *loader) {
  return core_prepare_runtime_retirement(base, span) &&
         yukizygisk::handoff::adopt_anonymous_loader(base, span, loader);
}

extern "C" int __real_dl_iterate_phdr(int (*)(dl_phdr_info *, size_t, void *),
                                      void *);
extern "C" __attribute__((visibility("hidden"))) int
core_system_iterate_phdr(int (*callback)(dl_phdr_info *, size_t, void *),
                         void *data) {
  return __real_dl_iterate_phdr(callback, data);
}
extern "C" __attribute__((visibility("hidden"))) int
__wrap_dl_iterate_phdr(int (*callback)(dl_phdr_info *, size_t, void *),
                       void *data) {
  dl_phdr_info info{};
  return own_headers(info) ? callback(&info, sizeof(info), data) : 0;
}
#if defined(__arm__)
extern "C" __attribute__((visibility("hidden")))
_Unwind_Ptr __wrap_dl_unwind_find_exidx(_Unwind_Ptr pc, int *count) {
  *count = 0;
  dl_phdr_info info{};
  if (!own_headers(info))
    return 0;
  bool contains = false;
  const ElfW(Phdr) *exidx = nullptr;
  for (size_t i = 0; i < info.dlpi_phnum; ++i) {
    const auto &ph = info.dlpi_phdr[i];
    if (ph.p_vaddr > UINTPTR_MAX - info.dlpi_addr)
      return 0;
    const uintptr_t begin = info.dlpi_addr + ph.p_vaddr;
    if (ph.p_type == PT_LOAD && pc >= begin && pc - begin < ph.p_memsz)
      contains = true;
    if (ph.p_type == PT_ARM_EXIDX)
      exidx = &ph;
  }
  if (!contains || exidx == nullptr || exidx->p_memsz % 8 != 0 ||
      exidx->p_memsz / 8 > INT_MAX)
    return 0;
  *count = static_cast<int>(exidx->p_memsz / 8);
  return info.dlpi_addr + exidx->p_vaddr;
}
#endif
#if defined(__ANDROID__)
// Host statics are released explicitly before returning outside the DSO.
extern "C" __attribute__((visibility("hidden"))) int
__cxa_atexit(void (*)(void *), void *, void *) {
  return 0;
}
extern "C" __attribute__((visibility("hidden"))) void __cxa_finalize(void *) {}
#endif
#endif
