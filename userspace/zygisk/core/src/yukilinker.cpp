/*
 * YukiZygisk in-memory ELF loader.
 *
 * The implementation is organized around the ELF image model: address space,
 * dynamic metadata, relocations, and per-image runtime services are independent
 * components. The public handle deliberately exposes no loader bookkeeping.
 */

#ifndef YUKILINKER_FULL
#define YUKILINKER_FULL 0
#endif // #ifndef YUKILINKER_FULL

#include "yukilinker.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <new>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "log.hpp"
#include "uapi/yukizygisk.h"

#if YUKILINKER_FULL
#include <pthread.h>
#endif // #if YUKILINKER_FULL

#if YUKILINKER_FULL && defined(__aarch64__)
struct YukiTlsFastConfig {
  uintptr_t enabled;
  uintptr_t key_index;
  uintptr_t key_sequence;
};

static_assert(offsetof(YukiTlsFastConfig, enabled) == 0);
static_assert(offsetof(YukiTlsFastConfig, key_index) == 8);
static_assert(offsetof(YukiTlsFastConfig, key_sequence) == 16);

extern "C" {
__attribute__((visibility("hidden"))) YukiTlsFastConfig yuki_tls_fast_config{};
}
#endif // #if YUKILINKER_FULL && defined(__aarch64__)

#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 0
#endif // #ifndef R_AARCH64_NONE
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#endif // #ifndef R_AARCH64_ABS64
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif // #ifndef R_AARCH64_GLOB_DAT
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif // #ifndef R_AARCH64_JUMP_SLOT
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif // #ifndef R_AARCH64_RELATIVE
#ifndef R_AARCH64_TLS_DTPREL64
#define R_AARCH64_TLS_DTPREL64 1028
#endif // #ifndef R_AARCH64_TLS_DTPREL64
#ifndef R_AARCH64_TLS_DTPMOD64
#define R_AARCH64_TLS_DTPMOD64 1029
#endif // #ifndef R_AARCH64_TLS_DTPMOD64
#ifndef R_AARCH64_TLS_TPREL64
#define R_AARCH64_TLS_TPREL64 1030
#endif // #ifndef R_AARCH64_TLS_TPREL64
#ifndef R_AARCH64_TLSDESC
#define R_AARCH64_TLSDESC 1031
#endif // #ifndef R_AARCH64_TLSDESC
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif // #ifndef R_AARCH64_IRELATIVE
#ifndef R_ARM_NONE
#define R_ARM_NONE 0
#endif // #ifndef R_ARM_NONE
#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif // #ifndef R_ARM_ABS32
#ifndef R_ARM_REL32
#define R_ARM_REL32 3
#endif // #ifndef R_ARM_REL32
#ifndef R_ARM_TLS_DESC
#define R_ARM_TLS_DESC 13
#endif // #ifndef R_ARM_TLS_DESC
#ifndef R_ARM_TLS_DTPMOD32
#define R_ARM_TLS_DTPMOD32 17
#endif // #ifndef R_ARM_TLS_DTPMOD32
#ifndef R_ARM_TLS_DTPOFF32
#define R_ARM_TLS_DTPOFF32 18
#endif // #ifndef R_ARM_TLS_DTPOFF32
#ifndef R_ARM_TLS_TPOFF32
#define R_ARM_TLS_TPOFF32 19
#endif // #ifndef R_ARM_TLS_TPOFF32
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif // #ifndef R_ARM_GLOB_DAT
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif // #ifndef R_ARM_JUMP_SLOT
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE 23
#endif // #ifndef R_ARM_RELATIVE
#ifndef R_ARM_IRELATIVE
#define R_ARM_IRELATIVE 160
#endif // #ifndef R_ARM_IRELATIVE
#ifndef STT_TLS
#define STT_TLS 6
#endif // #ifndef STT_TLS
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif // #ifndef STT_GNU_IFUNC
#ifndef DT_RELR
#define DT_RELR 0x6fffe000
#endif // #ifndef DT_RELR
#ifndef DT_RELRSZ
#define DT_RELRSZ 0x6fffe001
#endif // #ifndef DT_RELRSZ
#ifndef DT_RELRENT
#define DT_RELRENT 0x6fffe003
#endif // #ifndef DT_RELRENT
#ifndef DT_ANDROID_RELR
#define DT_ANDROID_RELR 0x6fffe000
#endif // #ifndef DT_ANDROID_RELR
#ifndef DT_ANDROID_RELRSZ
#define DT_ANDROID_RELRSZ 0x6fffe001
#endif // #ifndef DT_ANDROID_RELRSZ
#ifndef DT_ANDROID_RELRENT
#define DT_ANDROID_RELRENT 0x6fffe003
#endif // #ifndef DT_ANDROID_RELRENT

namespace yukilinker {
namespace {

#if defined(__LP64__)
using ElfRelocation = ElfW(Rela);
constexpr ElfW(Sxword) kDynamicRelTag = DT_RELA;
constexpr ElfW(Sxword) kDynamicRelSizeTag = DT_RELASZ;
constexpr ElfW(Sxword) kDynamicRelEntryTag = DT_RELAENT;
#else
using ElfRelocation = ElfW(Rel);
constexpr ElfW(Sxword) kDynamicRelTag = DT_REL;
constexpr ElfW(Sxword) kDynamicRelSizeTag = DT_RELSZ;
constexpr ElfW(Sxword) kDynamicRelEntryTag = DT_RELENT;
#endif

constexpr size_t kMetadataPageSize = size_t{64} * 1024;
size_t g_page_size = 0;

bool is_power_of_two(size_t value);

size_t loader_page_size() {
  if (g_page_size == 0) {
    unsigned long aux_page_size = getauxval(AT_PAGESZ);
    g_page_size = aux_page_size >= 4096 && is_power_of_two(aux_page_size)
                      ? static_cast<size_t>(aux_page_size)
                      : 4096;
  }
  return g_page_size;
}

bool add_overflow(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b)
    return true;
  *out = a + b;
  return false;
}

bool multiply_overflow(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a)
    return true;
  *out = a * b;
  return false;
}

bool add_signed_offset(uintptr_t base, ElfW(Sxword) offset, uintptr_t *out) {
  if (offset >= 0) {
    uintptr_t positive = static_cast<uintptr_t>(offset);
    if (positive > UINTPTR_MAX - base)
      return false;
    *out = base + positive;
    return true;
  }
  uintptr_t magnitude = static_cast<uintptr_t>(-(offset + 1)) + 1;
  if (magnitude > base)
    return false;
  *out = base - magnitude;
  return true;
}

bool is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

uintptr_t page_floor(uintptr_t value) {
  return value & ~(static_cast<uintptr_t>(loader_page_size()) - 1);
}

bool page_ceil(uintptr_t value, uintptr_t *out) {
  size_t page_size = loader_page_size();
  if (value > UINTPTR_MAX - (page_size - 1))
    return false;
  *out = page_floor(value + page_size - 1);
  return true;
}

size_t smaller(size_t a, size_t b) { return a < b ? a : b; }

struct MetadataPage {
  MetadataPage *next;
  size_t mapped_size;
  size_t cursor;
};

MetadataPage *g_metadata_pages = nullptr;

#if YUKILINKER_FULL
pthread_mutex_t g_metadata_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif // #if YUKILINKER_FULL

void *metadata_allocate(size_t bytes, size_t alignment) {
  if (bytes == 0 || !is_power_of_two(alignment))
    return nullptr;

#if YUKILINKER_FULL
  pthread_mutex_lock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL

  size_t header = sizeof(MetadataPage);
  size_t header_aligned =
      (header + alignof(max_align_t) - 1) & ~(alignof(max_align_t) - 1);
  MetadataPage *page = g_metadata_pages;
  while (page != nullptr) {
    size_t offset = (page->cursor + alignment - 1) & ~(alignment - 1);
    if (offset <= page->mapped_size && bytes <= page->mapped_size - offset) {
      page->cursor = offset + bytes;
      void *result = reinterpret_cast<uint8_t *>(page) + offset;
      memset(result, 0, bytes);
#if YUKILINKER_FULL
      pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
      return result;
    }
    page = page->next;
  }

  size_t required;
  if (add_overflow(header_aligned, alignment - 1, &required) ||
      add_overflow(required, bytes, &required)) {
#if YUKILINKER_FULL
    pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
    return nullptr;
  }
  uintptr_t rounded;
  if (!page_ceil(required, &rounded)) {
#if YUKILINKER_FULL
    pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
    return nullptr;
  }
  size_t mapping_size =
      rounded < kMetadataPageSize ? kMetadataPageSize : rounded;
  void *mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
#if YUKILINKER_FULL
    pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
    return nullptr;
  }

  page = static_cast<MetadataPage *>(mapping);
  page->next = g_metadata_pages;
  page->mapped_size = mapping_size;
  page->cursor = header_aligned;
  g_metadata_pages = page;

  size_t offset = (page->cursor + alignment - 1) & ~(alignment - 1);
  page->cursor = offset + bytes;
  void *result = reinterpret_cast<uint8_t *>(page) + offset;
  memset(result, 0, bytes);

#if YUKILINKER_FULL
  pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
  return result;
}

template <typename T> T *metadata_object() {
  void *memory = metadata_allocate(sizeof(T), alignof(T));
  return memory == nullptr ? nullptr : new (memory) T{};
}

struct AddressSpace {
  void *reservation = nullptr;
  size_t span = 0;
  uintptr_t lowest_vaddr = 0;
  uint8_t *bias = nullptr;
  ElfW(Phdr) *program_headers = nullptr;
  size_t program_header_count = 0;
  bool file_backed = false;
};

using LifecycleFunction = void (*)();

struct Lifecycle {
  LifecycleFunction init = nullptr;
  LifecycleFunction *init_array = nullptr;
  size_t init_count = 0;
  LifecycleFunction fini = nullptr;
  LifecycleFunction *fini_array = nullptr;
  size_t fini_count = 0;
  bool initialized = false;
};

struct DynamicSymbols {
  const ElfW(Sym) *entries = nullptr;
  size_t count = 0;
  const char *strings = nullptr;
  size_t string_bytes = 0;
  const uint32_t *index_slots = nullptr;
  size_t index_capacity = 0;
};

struct RelocationSet {
  const ElfRelocation *rel = nullptr;
  size_t rel_bytes = 0;
  const ElfRelocation *plt = nullptr;
  size_t plt_bytes = 0;
  const ElfW(Addr) *relr = nullptr;
  size_t relr_bytes = 0;
};

struct Dependency {
  void *system_handle = nullptr;
  Dependency *next = nullptr;
};

struct ImageState;

struct TlsTemplate {
  uintptr_t module_id = 0;
  uintptr_t generation = 0;
  const void *initial_bytes = nullptr;
  size_t file_bytes = 0;
  size_t memory_bytes = 0;
  size_t alignment = 1;
  bool active = false;
  ImageState *image = nullptr;
  TlsTemplate *next = nullptr;
};

#if YUKILINKER_FULL
struct ExitCallback {
  void (*function)(void *) = nullptr;
  void *argument = nullptr;
  void *dso = nullptr;
  ExitCallback *next = nullptr;
};
#endif // #if YUKILINKER_FULL

struct ImageState {
  SoHandle *public_handle = nullptr;
  AddressSpace memory;
  DynamicSymbols symbols;
  RelocationSet relocations;
  Lifecycle lifecycle;
  Dependency *dependencies = nullptr;
  TlsTemplate tls;
  const char *display_name = "";
  ImageState *previous = nullptr;
  ImageState *next = nullptr;
#if YUKILINKER_FULL
  ExitCallback *exit_callbacks = nullptr;
#endif // #if YUKILINKER_FULL
};

ImageState *g_first_image = nullptr;
ImageState *g_last_image = nullptr;

#if YUKILINKER_FULL
pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif // #if YUKILINKER_FULL

void registry_lock() {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_registry_mutex);
#endif // #if YUKILINKER_FULL
}

void registry_unlock() {
#if YUKILINKER_FULL
  pthread_mutex_unlock(&g_registry_mutex);
#endif // #if YUKILINKER_FULL
}

void register_image(ImageState *image) {
  registry_lock();
  image->previous = g_last_image;
  image->next = nullptr;
  if (g_last_image != nullptr)
    g_last_image->next = image;
  else
    g_first_image = image;
  g_last_image = image;
  registry_unlock();
}

void unregister_image(ImageState *image) {
  registry_lock();
  if (image->previous != nullptr)
    image->previous->next = image->next;
  else if (g_first_image == image)
    g_first_image = image->next;
  if (image->next != nullptr)
    image->next->previous = image->previous;
  else if (g_last_image == image)
    g_last_image = image->previous;
  image->previous = nullptr;
  image->next = nullptr;
  registry_unlock();
}

ImageState *state_of(SoHandle *handle) {
  return handle == nullptr ? nullptr
                           : static_cast<ImageState *>(handle->private_state);
}

bool address_space_contains(const AddressSpace &memory, const void *pointer,
                            size_t bytes) {
  if (memory.reservation == nullptr || pointer == nullptr)
    return false;
  uintptr_t begin = reinterpret_cast<uintptr_t>(memory.reservation);
  uintptr_t point = reinterpret_cast<uintptr_t>(pointer);
  if (point < begin || point - begin > memory.span)
    return false;
  return bytes <= memory.span - (point - begin);
}

bool image_contains(const ImageState *image, const void *pointer) {
  return image != nullptr && address_space_contains(image->memory, pointer, 1);
}

template <typename T>
T *runtime_pointer(ImageState *image, ElfW(Addr) virtual_address,
                   size_t count = 1) {
  size_t bytes;
  if (multiply_overflow(sizeof(T), count, &bytes))
    return nullptr;
  uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
  if (virtual_address > UINTPTR_MAX - bias)
    return nullptr;
  auto *pointer = reinterpret_cast<T *>(bias + virtual_address);
  return address_space_contains(image->memory, pointer, bytes) ? pointer
                                                               : nullptr;
}

int protection_for(uint32_t flags) {
  int protection = 0;
  if ((flags & PF_R) != 0)
    protection |= PROT_READ;
  if ((flags & PF_W) != 0)
    protection |= PROT_WRITE;
  if ((flags & PF_X) != 0)
    protection |= PROT_EXEC;
  return protection;
}

struct SourceLayout {
  const ElfW(Ehdr) *header = nullptr;
  const ElfW(Phdr) *program_headers = nullptr;
  size_t program_header_count = 0;
  uintptr_t lowest_vaddr = 0;
  uintptr_t highest_vaddr = 0;
  size_t dynamic_index = SIZE_MAX;
};

bool file_region_valid(size_t file_size, ElfW(Off) offset, ElfW(Xword) bytes) {
  if (offset > file_size)
    return false;
  return bytes <= file_size - static_cast<size_t>(offset);
}

bool inspect_source(const void *source, size_t file_size,
                    SourceLayout *layout) {
  if (file_size < sizeof(ElfW(Ehdr)))
    return false;
  const auto *header = static_cast<const ElfW(Ehdr) *>(source);
  if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
      header->e_ident[EI_CLASS] !=
          (sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32) ||
      header->e_ident[EI_DATA] != ELFDATA2LSB ||
      header->e_machine != (sizeof(void *) == 8 ? EM_AARCH64 : EM_ARM) ||
      header->e_type != ET_DYN || header->e_phentsize != sizeof(ElfW(Phdr)) ||
      header->e_phnum == 0)
    return false;

  size_t phdr_bytes;
  if (multiply_overflow(header->e_phnum, sizeof(ElfW(Phdr)), &phdr_bytes) ||
      !file_region_valid(file_size, header->e_phoff, phdr_bytes))
    return false;

  const auto *program_headers = reinterpret_cast<const ElfW(Phdr) *>(
      static_cast<const uint8_t *>(source) + header->e_phoff);
  uintptr_t low = UINTPTR_MAX;
  uintptr_t high = 0;
  uintptr_t previous_load_end = 0;
  bool saw_load = false;
  size_t dynamic_index = SIZE_MAX;

  for (size_t i = 0; i < header->e_phnum; ++i) {
    const ElfW(Phdr) &ph = program_headers[i];
    if (ph.p_type == PT_DYNAMIC) {
      if (dynamic_index != SIZE_MAX)
        return false;
      dynamic_index = i;
    }
    if (ph.p_type != PT_LOAD)
      continue;
    size_t page_size = loader_page_size();
    if (ph.p_filesz > ph.p_memsz ||
        !file_region_valid(file_size, ph.p_offset, ph.p_filesz) ||
        (ph.p_align > 1 && (!is_power_of_two(ph.p_align) ||
                            (ph.p_vaddr & (ph.p_align - 1)) !=
                                (ph.p_offset & (ph.p_align - 1)))) ||
        (ph.p_offset & (page_size - 1)) != (ph.p_vaddr & (page_size - 1)))
      return false;
    if (ph.p_memsz == 0)
      continue;
    uintptr_t segment_end;
    if (ph.p_vaddr > UINTPTR_MAX - ph.p_memsz ||
        !page_ceil(ph.p_vaddr + ph.p_memsz, &segment_end))
      return false;
    uintptr_t segment_begin = page_floor(ph.p_vaddr);
    // map_one_segment() replaces complete pages. Reject LOAD layouts whose
    // page-rounded ranges overlap so one segment cannot erase another.
    if (saw_load && segment_begin < previous_load_end)
      return false;
    previous_load_end = segment_end;
    saw_load = true;
    low = std::min(segment_begin, low);
    high = std::max(segment_end, high);
  }

  if (low == UINTPTR_MAX || high <= low || dynamic_index == SIZE_MAX)
    return false;

  layout->header = header;
  layout->program_headers = program_headers;
  layout->program_header_count = header->e_phnum;
  layout->lowest_vaddr = low;
  layout->highest_vaddr = high;
  layout->dynamic_index = dynamic_index;
  return true;
}

bool map_one_segment(int fd, const uint8_t *source, size_t file_size,
                     AddressSpace *memory, const ElfW(Phdr) & ph) {
  uintptr_t segment_page = page_floor(ph.p_vaddr);
  size_t page_prefix = static_cast<size_t>(ph.p_vaddr - segment_page);
  uintptr_t memory_end;
  if (!page_ceil(page_prefix + ph.p_memsz, &memory_end))
    return false;
  size_t mapped_bytes = static_cast<size_t>(memory_end);
  uintptr_t destination_value =
      reinterpret_cast<uintptr_t>(memory->bias) + segment_page;
  void *destination = reinterpret_cast<void *>(destination_value);

  if (!memory->file_backed) {
    // Request executable permission before replacing the reservation. This
    // makes an execmem denial fail the custom load cleanly instead of failing
    // only after relocation, when final protections are restored.
    int loading_protection =
        protection_for(ph.p_flags) | PROT_READ | PROT_WRITE;
    if (mmap(destination, mapped_bytes, loading_protection,
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
      return false;
    if (ph.p_filesz != 0) {
      if (!file_region_valid(file_size, ph.p_offset, ph.p_filesz))
        return false;
      memcpy(reinterpret_cast<uint8_t *>(destination) + page_prefix,
             source + ph.p_offset, ph.p_filesz);
    }
    return true;
  }

  uintptr_t file_end;
  if (!page_ceil(page_prefix + ph.p_filesz, &file_end))
    return false;
  size_t file_mapping_bytes = static_cast<size_t>(file_end);
  int final_protection = protection_for(ph.p_flags);
  ElfW(Off) aligned_file_offset = ph.p_offset - page_prefix;

  if (file_mapping_bytes != 0 &&
      mmap(destination, file_mapping_bytes, final_protection,
           MAP_FIXED | MAP_PRIVATE, fd, aligned_file_offset) == MAP_FAILED)
    return false;

  size_t data_end = page_prefix + static_cast<size_t>(ph.p_filesz);
  size_t zero_available =
      file_mapping_bytes > data_end ? file_mapping_bytes - data_end : 0;
  size_t bss_bytes = static_cast<size_t>(ph.p_memsz - ph.p_filesz);
  size_t zero_bytes = smaller(zero_available, bss_bytes);
  if (zero_bytes != 0) {
    if ((final_protection & PROT_WRITE) == 0)
      return false;
    memset(reinterpret_cast<uint8_t *>(destination) + data_end, 0, zero_bytes);
  }

  if (mapped_bytes > file_mapping_bytes) {
    void *bss = reinterpret_cast<uint8_t *>(destination) + file_mapping_bytes;
    if (mmap(bss, mapped_bytes - file_mapping_bytes, final_protection,
             MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
      return false;
  }
  return true;
}

bool create_address_space(int fd, const uint8_t *source, size_t file_size,
                          const SourceLayout &layout, bool file_backed,
                          AddressSpace *memory) {
  size_t span = layout.highest_vaddr - layout.lowest_vaddr;
  void *reservation =
      mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED)
    return false;

  memory->reservation = reservation;
  memory->span = span;
  memory->lowest_vaddr = layout.lowest_vaddr;
  memory->bias = reinterpret_cast<uint8_t *>(
      reinterpret_cast<uintptr_t>(reservation) - layout.lowest_vaddr);
  memory->program_header_count = layout.program_header_count;
  memory->file_backed = file_backed;

  size_t phdr_bytes;
  if (multiply_overflow(layout.program_header_count, sizeof(ElfW(Phdr)),
                        &phdr_bytes)) {
    munmap(reservation, span);
    return false;
  }
  memory->program_headers = static_cast<ElfW(Phdr) *>(
      metadata_allocate(phdr_bytes, alignof(ElfW(Phdr))));
  if (memory->program_headers == nullptr) {
    munmap(reservation, span);
    return false;
  }
  memcpy(memory->program_headers, layout.program_headers, phdr_bytes);

  for (size_t i = 0; i < layout.program_header_count; ++i) {
    const ElfW(Phdr) &ph = layout.program_headers[i];
    if (ph.p_type == PT_LOAD && ph.p_memsz != 0 &&
        !map_one_segment(fd, source, file_size, memory, ph)) {
      munmap(reservation, span);
      memory->reservation = nullptr;
      memory->span = 0;
      return false;
    }
  }
  return true;
}

bool restore_anonymous_protections(ImageState *image) {
  if (image->memory.file_backed)
    return true;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) &ph = image->memory.program_headers[i];
    if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
      continue;
    uintptr_t segment_page = page_floor(ph.p_vaddr);
    size_t prefix = static_cast<size_t>(ph.p_vaddr - segment_page);
    uintptr_t rounded;
    if (!page_ceil(prefix + ph.p_memsz, &rounded))
      return false;
    auto *address = runtime_pointer<uint8_t>(image, segment_page, rounded);
    if (address == nullptr)
      return false;
    int protection = protection_for(ph.p_flags);
    if (mprotect(address, rounded, protection) != 0)
      return false;
    if ((protection & PROT_EXEC) != 0)
      __builtin___clear_cache(reinterpret_cast<char *>(address),
                              reinterpret_cast<char *>(address + rounded));
  }
  return true;
}

bool protect_relro(ImageState *image) {
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) &ph = image->memory.program_headers[i];
    if (ph.p_type != PT_GNU_RELRO || ph.p_memsz == 0)
      continue;
    uintptr_t bias = reinterpret_cast<uintptr_t>(image->memory.bias);
    if (ph.p_vaddr > UINTPTR_MAX - bias)
      return false;
    uintptr_t unrounded_start = bias + ph.p_vaddr;
    if (ph.p_memsz > UINTPTR_MAX - unrounded_start)
      return false;
    uintptr_t start = page_floor(unrounded_start);
    uintptr_t unrounded_end = unrounded_start + ph.p_memsz;
    uintptr_t end;
    if (!page_ceil(unrounded_end, &end) || end <= start)
      return false;
    auto *address = reinterpret_cast<void *>(start);
    if (!address_space_contains(image->memory, address, end - start) ||
        mprotect(address, end - start, PROT_READ) != 0)
      return false;
  }
  return true;
}

struct DynamicParse {
  const ElfW(Dyn) *entries = nullptr;
  size_t entry_limit = 0;
  const uint32_t *sysv_hash = nullptr;
  const uint32_t *gnu_hash = nullptr;
  size_t init_array_bytes = 0;
  size_t fini_array_bytes = 0;
  size_t symbol_entry_size = sizeof(ElfW(Sym));
  size_t rel_entry_size = sizeof(ElfRelocation);
  size_t relr_entry_size = sizeof(ElfW(Addr));
  ElfW(Sxword) plt_relocation_kind = kDynamicRelTag;
  bool terminated = false;
};

bool mapped_bytes(ImageState *image, const void *pointer, size_t bytes) {
  return address_space_contains(image->memory, pointer, bytes);
}

template <typename T>
const T *dynamic_pointer(ImageState *image, const ElfW(Dyn) & entry) {
  return runtime_pointer<T>(image, entry.d_un.d_ptr);
}

bool parse_dynamic_tags(ImageState *image, size_t dynamic_index,
                        DynamicParse *parse) {
  const ElfW(Phdr) &dynamic_ph = image->memory.program_headers[dynamic_index];
  parse->entries = runtime_pointer<ElfW(Dyn)>(image, dynamic_ph.p_vaddr);
  parse->entry_limit = dynamic_ph.p_memsz / sizeof(ElfW(Dyn));
  if (parse->entries == nullptr || parse->entry_limit == 0 ||
      !mapped_bytes(image, parse->entries,
                    parse->entry_limit * sizeof(ElfW(Dyn))))
    return false;

  for (size_t i = 0; i < parse->entry_limit; ++i) {
    const ElfW(Dyn) &entry = parse->entries[i];
    if (entry.d_tag == DT_NULL) {
      parse->terminated = true;
      break;
    }
    switch (entry.d_tag) {
    case DT_SYMTAB:
      image->symbols.entries = dynamic_pointer<ElfW(Sym)>(image, entry);
      break;
    case DT_STRTAB:
      image->symbols.strings = dynamic_pointer<char>(image, entry);
      break;
    case DT_STRSZ:
      image->symbols.string_bytes = entry.d_un.d_val;
      break;
    case DT_SYMENT:
      parse->symbol_entry_size = entry.d_un.d_val;
      break;
    case DT_HASH:
      parse->sysv_hash = dynamic_pointer<uint32_t>(image, entry);
      break;
    case DT_GNU_HASH:
      parse->gnu_hash = dynamic_pointer<uint32_t>(image, entry);
      break;
    case kDynamicRelTag:
      image->relocations.rel = dynamic_pointer<ElfRelocation>(image, entry);
      break;
    case kDynamicRelSizeTag:
      image->relocations.rel_bytes = entry.d_un.d_val;
      break;
    case kDynamicRelEntryTag:
      parse->rel_entry_size = entry.d_un.d_val;
      break;
    case DT_JMPREL:
      image->relocations.plt = dynamic_pointer<ElfRelocation>(image, entry);
      break;
    case DT_PLTRELSZ:
      image->relocations.plt_bytes = entry.d_un.d_val;
      break;
    case DT_PLTREL:
      parse->plt_relocation_kind = entry.d_un.d_val;
      break;
    case DT_RELR:
#if DT_ANDROID_RELR != DT_RELR
    case DT_ANDROID_RELR:
#endif // #if DT_ANDROID_RELR != DT_RELR
      image->relocations.relr = dynamic_pointer<ElfW(Addr)>(image, entry);
      break;
    case DT_RELRSZ:
#if DT_ANDROID_RELRSZ != DT_RELRSZ
    case DT_ANDROID_RELRSZ:
#endif // #if DT_ANDROID_RELRSZ != DT_RELRSZ
      image->relocations.relr_bytes = entry.d_un.d_val;
      break;
    case DT_RELRENT:
#if DT_ANDROID_RELRENT != DT_RELRENT
    case DT_ANDROID_RELRENT:
#endif // #if DT_ANDROID_RELRENT != DT_RELRENT
      parse->relr_entry_size = entry.d_un.d_val;
      break;
    case DT_INIT: {
      auto *pointer = entry.d_un.d_ptr == 0
                          ? nullptr
                          : runtime_pointer<uint8_t>(image, entry.d_un.d_ptr);
      if (entry.d_un.d_ptr != 0 && pointer == nullptr)
        return false;
      image->lifecycle.init = reinterpret_cast<LifecycleFunction>(pointer);
      break;
    }
    case DT_INIT_ARRAY:
      image->lifecycle.init_array =
          runtime_pointer<LifecycleFunction>(image, entry.d_un.d_ptr);
      if (entry.d_un.d_ptr != 0 && image->lifecycle.init_array == nullptr)
        return false;
      break;
    case DT_INIT_ARRAYSZ:
      parse->init_array_bytes = entry.d_un.d_val;
      break;
    case DT_FINI: {
      auto *pointer = entry.d_un.d_ptr == 0
                          ? nullptr
                          : runtime_pointer<uint8_t>(image, entry.d_un.d_ptr);
      if (entry.d_un.d_ptr != 0 && pointer == nullptr)
        return false;
      image->lifecycle.fini = reinterpret_cast<LifecycleFunction>(pointer);
      break;
    }
    case DT_FINI_ARRAY:
      image->lifecycle.fini_array =
          runtime_pointer<LifecycleFunction>(image, entry.d_un.d_ptr);
      if (entry.d_un.d_ptr != 0 && image->lifecycle.fini_array == nullptr)
        return false;
      break;
    case DT_FINI_ARRAYSZ:
      parse->fini_array_bytes = entry.d_un.d_val;
      break;
    default:
      break;
    }
  }

  if (!parse->terminated || image->symbols.entries == nullptr ||
      image->symbols.strings == nullptr || image->symbols.string_bytes == 0 ||
      parse->symbol_entry_size != sizeof(ElfW(Sym)) ||
      parse->rel_entry_size != sizeof(ElfRelocation) ||
      parse->relr_entry_size != sizeof(ElfW(Addr)) ||
      parse->plt_relocation_kind != kDynamicRelTag)
    return false;

  if (parse->init_array_bytes % sizeof(LifecycleFunction) != 0 ||
      parse->fini_array_bytes % sizeof(LifecycleFunction) != 0)
    return false;
  image->lifecycle.init_count =
      parse->init_array_bytes / sizeof(LifecycleFunction);
  image->lifecycle.fini_count =
      parse->fini_array_bytes / sizeof(LifecycleFunction);
  if ((parse->init_array_bytes != 0 &&
       (image->lifecycle.init_array == nullptr ||
        !mapped_bytes(image, image->lifecycle.init_array,
                      parse->init_array_bytes))) ||
      (parse->fini_array_bytes != 0 &&
       (image->lifecycle.fini_array == nullptr ||
        !mapped_bytes(image, image->lifecycle.fini_array,
                      parse->fini_array_bytes))))
    return false;

  if (!mapped_bytes(image, image->symbols.strings, image->symbols.string_bytes))
    return false;
  if ((image->relocations.rel_bytes != 0 &&
       (image->relocations.rel == nullptr ||
        !mapped_bytes(image, image->relocations.rel,
                      image->relocations.rel_bytes))) ||
      (image->relocations.plt_bytes != 0 &&
       (image->relocations.plt == nullptr ||
        !mapped_bytes(image, image->relocations.plt,
                      image->relocations.plt_bytes))) ||
      (image->relocations.relr_bytes != 0 &&
       (image->relocations.relr == nullptr ||
        !mapped_bytes(image, image->relocations.relr,
                      image->relocations.relr_bytes))))
    return false;
  return true;
}

size_t symbol_count_from_sysv(ImageState *image, const uint32_t *table) {
  if (table == nullptr || !mapped_bytes(image, table, 2 * sizeof(uint32_t)))
    return 0;
  uint32_t bucket_count = table[0];
  uint32_t chain_count = table[1];
  size_t words;
  if (add_overflow(2, bucket_count, &words) ||
      add_overflow(words, chain_count, &words) ||
      multiply_overflow(words, sizeof(uint32_t), &words) ||
      !mapped_bytes(image, table, words))
    return 0;
  return chain_count;
}

size_t symbol_count_from_gnu(ImageState *image, const uint32_t *table) {
  if (table == nullptr || !mapped_bytes(image, table, 4 * sizeof(uint32_t)))
    return 0;
  uint32_t bucket_count = table[0];
  uint32_t first_hashed_symbol = table[1];
  uint32_t bloom_words = table[2];
  if (bucket_count == 0 || bloom_words == 0)
    return 0;

  const auto *bloom = reinterpret_cast<const ElfW(Addr) *>(table + 4);
  size_t bloom_bytes;
  if (multiply_overflow(bloom_words, sizeof(ElfW(Addr)), &bloom_bytes) ||
      !mapped_bytes(image, bloom, bloom_bytes))
    return 0;
  const auto *buckets = reinterpret_cast<const uint32_t *>(
      reinterpret_cast<const uint8_t *>(bloom) + bloom_bytes);
  size_t bucket_bytes;
  if (multiply_overflow(bucket_count, sizeof(uint32_t), &bucket_bytes) ||
      !mapped_bytes(image, buckets, bucket_bytes))
    return 0;
  const uint32_t *chains = buckets + bucket_count;

  uint32_t largest_bucket = 0;
  for (uint32_t i = 0; i < bucket_count; ++i)
    largest_bucket = std::max(buckets[i], largest_bucket);
  if (largest_bucket < first_hashed_symbol)
    return first_hashed_symbol;

  size_t symbol_index = largest_bucket;
  for (;;) {
    size_t chain_index = symbol_index - first_hashed_symbol;
    const uint32_t *chain = chains + chain_index;
    if (!mapped_bytes(image, chain, sizeof(*chain)))
      return 0;
    uint32_t value = *chain;
    ++symbol_index;
    if ((value & 1U) != 0)
      return symbol_index;
  }
}

bool establish_symbol_count(ImageState *image, const DynamicParse &parse) {
  size_t sysv_count = symbol_count_from_sysv(image, parse.sysv_hash);
  size_t gnu_count = symbol_count_from_gnu(image, parse.gnu_hash);
  image->symbols.count = sysv_count > gnu_count ? sysv_count : gnu_count;
  size_t symbol_bytes;
  return image->symbols.count != 0 &&
         !multiply_overflow(image->symbols.count, sizeof(ElfW(Sym)),
                            &symbol_bytes) &&
         mapped_bytes(image, image->symbols.entries, symbol_bytes);
}

bool symbol_name_valid(const DynamicSymbols &symbols,
                       const ElfW(Sym) & symbol) {
  if (symbol.st_name >= symbols.string_bytes)
    return false;
  const char *name = symbols.strings + symbol.st_name;
  return memchr(name, '\0', symbols.string_bytes - symbol.st_name) != nullptr;
}

bool requires_system_tls_runtime(const ImageState *image) {
  const DynamicSymbols &symbols = image->symbols;
  bool uses_thread_atexit = false;
  bool uses_emulated_tls = false;
  for (size_t i = 0; i < symbols.count; ++i) {
    const ElfW(Sym) &symbol = symbols.entries[i];
    if (!symbol_name_valid(symbols, symbol))
      continue;
    const char *name = symbols.strings + symbol.st_name;
    if (symbol.st_shndx == SHN_UNDEF &&
        ELF64_ST_TYPE(symbol.st_info) == STT_TLS) {
      ZLOGW("yukilinker: external TLS symbol requires system linker: %s", name);
      return true;
    }
    if (symbol.st_shndx == SHN_UNDEF &&
        (strcmp(name, "__cxa_thread_atexit") == 0 ||
         strcmp(name, "__cxa_thread_atexit_impl") == 0))
      uses_thread_atexit = true;
    if (strcmp(name, "__emutls_get_address") == 0 ||
        strncmp(name, "__emutls_v.", sizeof("__emutls_v.") - 1) == 0)
      uses_emulated_tls = true;
  }
  if (uses_thread_atexit &&
      (image->tls.memory_bytes != 0 || uses_emulated_tls)) {
    ZLOGW("yukilinker: TLS destructor runtime requires system linker");
    return true;
  }
  return false;
}

uint32_t symbol_name_hash(const char *name) {
  uint32_t hash = 2166136261U;
  for (const auto *cursor = reinterpret_cast<const uint8_t *>(name);
       *cursor != 0; ++cursor) {
    hash ^= *cursor;
    hash *= 16777619U;
  }
  return hash;
}

void build_symbol_index(ImageState *image) {
  DynamicSymbols &symbols = image->symbols;
  if (symbols.count > UINT32_MAX)
    return;

  size_t defined_count = 0;
  for (size_t i = 0; i < symbols.count; ++i) {
    const ElfW(Sym) &candidate = symbols.entries[i];
    if (candidate.st_shndx != SHN_UNDEF &&
        symbol_name_valid(symbols, candidate))
      ++defined_count;
  }
  if (defined_count == 0)
    return;

  size_t target_capacity;
  if (multiply_overflow(defined_count, 2, &target_capacity))
    return;
  size_t capacity = 8;
  while (capacity < target_capacity) {
    if (capacity > SIZE_MAX / 2)
      return;
    capacity *= 2;
  }

  size_t index_bytes;
  if (multiply_overflow(capacity, sizeof(uint32_t), &index_bytes))
    return;
  auto *slots = static_cast<uint32_t *>(
      metadata_allocate(index_bytes, alignof(uint32_t)));
  if (slots == nullptr)
    return;

  size_t mask = capacity - 1;
  for (size_t i = 0; i < symbols.count; ++i) {
    const ElfW(Sym) &candidate = symbols.entries[i];
    if (candidate.st_shndx == SHN_UNDEF ||
        !symbol_name_valid(symbols, candidate))
      continue;
    const char *candidate_name = symbols.strings + candidate.st_name;
    size_t slot = symbol_name_hash(candidate_name) & mask;
    for (size_t probe = 0; probe < capacity; ++probe) {
      uint32_t encoded = slots[slot];
      if (encoded == 0) {
        slots[slot] = static_cast<uint32_t>(i + 1);
        break;
      }
      const ElfW(Sym) &existing = symbols.entries[encoded - 1];
      if (strcmp(symbols.strings + existing.st_name, candidate_name) == 0)
        break;
      slot = (slot + 1) & mask;
    }
  }
  symbols.index_slots = slots;
  symbols.index_capacity = capacity;
}

const ElfW(Sym) *
    find_defined_symbol(const ImageState *image, const char *name) {
  if (image == nullptr || name == nullptr)
    return nullptr;
  const DynamicSymbols &symbols = image->symbols;
  if (symbols.index_slots != nullptr && symbols.index_capacity != 0) {
    size_t mask = symbols.index_capacity - 1;
    size_t slot = symbol_name_hash(name) & mask;
    for (size_t probe = 0; probe < symbols.index_capacity; ++probe) {
      uint32_t encoded = symbols.index_slots[slot];
      if (encoded == 0)
        return nullptr;
      const ElfW(Sym) &candidate = symbols.entries[encoded - 1];
      if (strcmp(symbols.strings + candidate.st_name, name) == 0)
        return &candidate;
      slot = (slot + 1) & mask;
    }
    return nullptr;
  }
  for (size_t i = 0; i < image->symbols.count; ++i) {
    const ElfW(Sym) &candidate = image->symbols.entries[i];
    if (candidate.st_shndx == SHN_UNDEF ||
        !symbol_name_valid(image->symbols, candidate))
      continue;
    if (strcmp(image->symbols.strings + candidate.st_name, name) == 0)
      return &candidate;
  }
  return nullptr;
}

const ElfW(Sym) * symbol_at(const ImageState *image, uint32_t index) {
  return image != nullptr && index < image->symbols.count
             ? &image->symbols.entries[index]
             : nullptr;
}

bool open_dependencies(ImageState *image, const DynamicParse &parse) {
  Dependency **tail = &image->dependencies;
  for (size_t i = 0; i < parse.entry_limit; ++i) {
    const ElfW(Dyn) &entry = parse.entries[i];
    if (entry.d_tag == DT_NULL)
      break;
    if (entry.d_tag != DT_NEEDED)
      continue;
    if (entry.d_un.d_val >= image->symbols.string_bytes)
      return false;
    const char *name = image->symbols.strings + entry.d_un.d_val;
    if (memchr(name, '\0', image->symbols.string_bytes - entry.d_un.d_val) ==
        nullptr)
      return false;
    Dependency *dependency = metadata_object<Dependency>();
    if (dependency == nullptr)
      return false;
    dependency->system_handle = ::dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (dependency->system_handle == nullptr)
      ZLOGW("yukilinker: dependency unavailable: %s", name);
    *tail = dependency;
    tail = &dependency->next;
  }
  return true;
}

void close_dependencies(ImageState *image) {
  for (Dependency *dependency = image->dependencies; dependency != nullptr;
       dependency = dependency->next) {
    if (dependency->system_handle != nullptr) {
      ::dlclose(dependency->system_handle);
      dependency->system_handle = nullptr;
    }
  }
}

#if YUKILINKER_FULL
pthread_mutex_t g_exit_mutex = PTHREAD_MUTEX_INITIALIZER;
ImageState *g_initializing_image = nullptr;

ImageState *find_exit_owner(void *dso) {
  if (dso != nullptr) {
    registry_lock();
    for (ImageState *image = g_first_image; image != nullptr;
         image = image->next) {
      if (image_contains(image, dso)) {
        registry_unlock();
        return image;
      }
    }
    registry_unlock();
  }
  return g_initializing_image;
}

void drain_exit_callbacks(ImageState *image, void *dso) {
  if (image == nullptr)
    return;
  for (;;) {
    pthread_mutex_lock(&g_exit_mutex);
    ExitCallback **link = &image->exit_callbacks;
    while (*link != nullptr && dso != nullptr && (*link)->dso != dso)
      link = &(*link)->next;
    ExitCallback *callback = *link;
    if (callback != nullptr)
      *link = callback->next;
    pthread_mutex_unlock(&g_exit_mutex);
    if (callback == nullptr)
      break;
    callback->function(callback->argument);
    free(callback);
  }
}

void discard_exit_callbacks(ImageState *image) {
  pthread_mutex_lock(&g_exit_mutex);
  ExitCallback *callback = image->exit_callbacks;
  image->exit_callbacks = nullptr;
  pthread_mutex_unlock(&g_exit_mutex);
  while (callback != nullptr) {
    ExitCallback *next = callback->next;
    free(callback);
    callback = next;
  }
}

int module_cxa_atexit(void (*function)(void *), void *argument, void *dso) {
  if (function == nullptr)
    return -1;
  pthread_mutex_lock(&g_exit_mutex);
  ImageState *owner = find_exit_owner(dso);
  if (owner == nullptr) {
    pthread_mutex_unlock(&g_exit_mutex);
    return -1;
  }
  auto *callback = static_cast<ExitCallback *>(malloc(sizeof(ExitCallback)));
  if (callback == nullptr) {
    pthread_mutex_unlock(&g_exit_mutex);
    return -1;
  }
  callback->function = function;
  callback->argument = argument;
  callback->dso = dso;
  callback->next = owner->exit_callbacks;
  owner->exit_callbacks = callback;
  pthread_mutex_unlock(&g_exit_mutex);
  return 0;
}

void module_cxa_finalize(void *dso) {
  pthread_mutex_lock(&g_exit_mutex);
  ImageState *owner = find_exit_owner(dso);
  pthread_mutex_unlock(&g_exit_mutex);
  drain_exit_callbacks(owner, dso);
}
#else
int module_cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void module_cxa_finalize(void *) {}
void discard_exit_callbacks(ImageState *) {}
#endif // #if YUKILINKER_FULL

extern "C" int yuki_thread_atexit_noop(void (*)(void *), void *, void *) {
  return 0;
}

#if YUKILINKER_FULL
struct TlsIndex {
  unsigned long module;
  unsigned long offset;
};

struct TlsDescriptorIndex {
  uintptr_t module;
  uintptr_t offset;
  uintptr_t generation;
};

struct ThreadTlsBlock {
  uintptr_t generation = 0;
  void *storage = nullptr;
  ThreadTlsBlock *next = nullptr;
  uintptr_t module_id = 0;
};

struct ThreadTlsState {
  size_t block_capacity = 0;
  ThreadTlsBlock **blocks = nullptr;
  ThreadTlsBlock *first = nullptr;
};

static_assert(offsetof(ThreadTlsState, block_capacity) == 0);
static_assert(offsetof(ThreadTlsState, blocks) == sizeof(size_t));
static_assert(offsetof(ThreadTlsBlock, generation) == 0);
static_assert(offsetof(ThreadTlsBlock, storage) == sizeof(uintptr_t));
static_assert(offsetof(TlsDescriptorIndex, module) == 0);
static_assert(offsetof(TlsDescriptorIndex, offset) == sizeof(uintptr_t));
static_assert(offsetof(TlsDescriptorIndex, generation) ==
              2 * sizeof(uintptr_t));

constexpr size_t kMaxTlsModuleSlots = 1024;
TlsTemplate *g_tls_templates = nullptr;
uintptr_t g_tls_module_generations[kMaxTlsModuleSlots + 1]{};
uintptr_t g_next_tls_generation = 1;
pthread_mutex_t g_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t g_thread_tls_key;
pthread_once_t g_thread_tls_once = PTHREAD_ONCE_INIT;
bool g_thread_tls_key_valid = false;
bool g_tls_stopped = false;

constexpr uint32_t kPthreadKeyValidFlag = 1U << 31;

#if defined(__aarch64__)
struct BionicPthreadKeyData {
  uintptr_t sequence;
  void *data;
};

static_assert(sizeof(BionicPthreadKeyData) == 16);
static_assert(offsetof(BionicPthreadKeyData, data) == 8);

void configure_tls_fast_path() {
  uint32_t encoded_key = static_cast<uint32_t>(g_thread_tls_key);
  if ((encoded_key & kPthreadKeyValidFlag) == 0)
    return;
  uintptr_t key_index = encoded_key & ~kPthreadKeyValidFlag;

  static uintptr_t probe_marker;
  if (pthread_setspecific(g_thread_tls_key, &probe_marker) != 0)
    return;

  auto **thread_slots = static_cast<void **>(__builtin_thread_pointer());
  auto *key_data = static_cast<BionicPthreadKeyData *>(thread_slots[-1]);
  bool valid = key_data != nullptr &&
               key_data[key_index].data == &probe_marker &&
               pthread_getspecific(g_thread_tls_key) == &probe_marker;
  uintptr_t sequence = valid ? key_data[key_index].sequence : 0;

  pthread_setspecific(g_thread_tls_key, nullptr);
  if (!valid || sequence == 0 || key_data[key_index].data != nullptr ||
      key_data[key_index].sequence != sequence)
    return;

  yuki_tls_fast_config.key_index = key_index;
  yuki_tls_fast_config.key_sequence = sequence;
  __atomic_store_n(&yuki_tls_fast_config.enabled, uintptr_t{1},
                   __ATOMIC_RELEASE);
}

void disable_tls_fast_path() {
  __atomic_store_n(&yuki_tls_fast_config.enabled, uintptr_t{0},
                   __ATOMIC_RELEASE);
}

ThreadTlsState *fast_thread_tls_state() {
  if (__atomic_load_n(&yuki_tls_fast_config.enabled, __ATOMIC_ACQUIRE) != 1)
    return nullptr;
  auto **thread_slots = static_cast<void **>(__builtin_thread_pointer());
  auto *key_data = static_cast<BionicPthreadKeyData *>(thread_slots[-1]);
  if (key_data == nullptr)
    return nullptr;
  BionicPthreadKeyData &entry = key_data[yuki_tls_fast_config.key_index];
  if (entry.sequence != yuki_tls_fast_config.key_sequence)
    return nullptr;
  return static_cast<ThreadTlsState *>(entry.data);
}
#else
void configure_tls_fast_path() {}
void disable_tls_fast_path() {}
ThreadTlsState *fast_thread_tls_state() { return nullptr; }
#endif // #if defined(__aarch64__)

void destroy_thread_tls_state(void *opaque) {
  auto *state = static_cast<ThreadTlsState *>(opaque);
  if (state == nullptr)
    return;
  ThreadTlsBlock *block = state->first;
  while (block != nullptr) {
    ThreadTlsBlock *next = block->next;
    free(block->storage);
    free(block);
    block = next;
  }
  free(state->blocks);
  free(state);
}

void initialize_thread_tls_key() {
  bool valid =
      pthread_key_create(&g_thread_tls_key, destroy_thread_tls_state) == 0;
  if (valid)
    configure_tls_fast_path();
  __atomic_store_n(&g_thread_tls_key_valid, valid, __ATOMIC_RELEASE);
}

bool ensure_thread_tls_key() {
  pthread_once(&g_thread_tls_once, initialize_thread_tls_key);
  return __atomic_load_n(&g_thread_tls_key_valid, __ATOMIC_ACQUIRE);
}

ThreadTlsState *thread_tls_state() {
  if (g_tls_stopped)
    return nullptr;
  if (!ensure_thread_tls_key())
    return nullptr;
  auto *state =
      static_cast<ThreadTlsState *>(pthread_getspecific(g_thread_tls_key));
  if (state != nullptr)
    return state;
  state = static_cast<ThreadTlsState *>(calloc(1, sizeof(ThreadTlsState)));
  if (state == nullptr)
    return nullptr;
  if (pthread_setspecific(g_thread_tls_key, state) != 0) {
    free(state);
    return nullptr;
  }
  return state;
}

TlsTemplate *find_tls_template(uintptr_t module_id) {
  for (TlsTemplate *candidate = g_tls_templates; candidate != nullptr;
       candidate = candidate->next)
    if (candidate->active && candidate->module_id == module_id)
      return candidate;
  return nullptr;
}

ThreadTlsBlock *find_thread_block(ThreadTlsState *thread, uintptr_t module_id) {
  if (module_id >= thread->block_capacity)
    return nullptr;
  return thread->blocks[module_id];
}

bool ensure_thread_block_capacity(ThreadTlsState *thread, uintptr_t module_id) {
  if (module_id < thread->block_capacity)
    return true;
  if (module_id == UINTPTR_MAX)
    return false;

  size_t required = static_cast<size_t>(module_id + 1);
  size_t capacity = thread->block_capacity == 0 ? 8 : thread->block_capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }

  size_t bytes;
  if (multiply_overflow(capacity, sizeof(ThreadTlsBlock *), &bytes))
    return false;
  auto **blocks =
      static_cast<ThreadTlsBlock **>(realloc(thread->blocks, bytes));
  if (blocks == nullptr)
    return false;
  memset(blocks + thread->block_capacity, 0,
         (capacity - thread->block_capacity) * sizeof(ThreadTlsBlock *));
  thread->blocks = blocks;
  thread->block_capacity = capacity;
  return true;
}

void *allocate_thread_storage(const TlsTemplate &tls) {
  void *storage = nullptr;
  size_t alignment =
      tls.alignment < sizeof(void *) ? sizeof(void *) : tls.alignment;
  if (posix_memalign(&storage, alignment, tls.memory_bytes) != 0)
    return nullptr;
  memset(storage, 0, tls.memory_bytes);
  if (tls.file_bytes != 0)
    memcpy(storage, tls.initial_bytes, tls.file_bytes);
  return storage;
}

ThreadTlsBlock *create_thread_block(const TlsTemplate &tls) {
  auto *block =
      static_cast<ThreadTlsBlock *>(calloc(1, sizeof(ThreadTlsBlock)));
  if (block == nullptr)
    return nullptr;
  block->storage = allocate_thread_storage(tls);
  if (block->storage == nullptr) {
    free(block);
    return nullptr;
  }
  block->generation = tls.generation;
  block->module_id = tls.module_id;
  return block;
}

bool reset_thread_block(ThreadTlsBlock *block, const TlsTemplate &tls) {
  void *storage = allocate_thread_storage(tls);
  if (storage == nullptr)
    return false;
  free(block->storage);
  block->storage = storage;
  block->generation = tls.generation;
  block->module_id = tls.module_id;
  return true;
}

uintptr_t active_tls_generation(uintptr_t module_id) {
  if (module_id == 0 || module_id > kMaxTlsModuleSlots)
    return 0;
  return __atomic_load_n(&g_tls_module_generations[module_id],
                         __ATOMIC_ACQUIRE);
}

bool activate_tls(ImageState *image) {
  TlsTemplate &tls = image->tls;
  if (tls.memory_bytes == 0)
    return true;
  if (tls.file_bytes > tls.memory_bytes ||
      !is_power_of_two(tls.alignment == 0 ? 1 : tls.alignment))
    return false;
  if (tls.alignment == 0)
    tls.alignment = 1;
  if (!ensure_thread_tls_key())
    return false;

  pthread_mutex_lock(&g_tls_mutex);
  if (g_tls_stopped) {
    pthread_mutex_unlock(&g_tls_mutex);
    return false;
  }
  uintptr_t module_id = 1;
  while (module_id <= kMaxTlsModuleSlots &&
         __atomic_load_n(&g_tls_module_generations[module_id],
                         __ATOMIC_RELAXED) != 0)
    ++module_id;
  if (module_id > kMaxTlsModuleSlots) {
    pthread_mutex_unlock(&g_tls_mutex);
    return false;
  }
  uintptr_t generation = g_next_tls_generation++;
  if (generation == 0)
    generation = g_next_tls_generation++;
  tls.module_id = module_id;
  tls.generation = generation;
  tls.image = image;
  tls.active = true;
  tls.next = g_tls_templates;
  g_tls_templates = &tls;
  __atomic_store_n(&g_tls_module_generations[module_id], generation,
                   __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_tls_mutex);
  return true;
}

void deactivate_tls(ImageState *image) {
  if (image == nullptr || !image->tls.active)
    return;
  pthread_mutex_lock(&g_tls_mutex);
  TlsTemplate **link = &g_tls_templates;
  while (*link != nullptr && *link != &image->tls)
    link = &(*link)->next;
  if (*link == &image->tls)
    *link = image->tls.next;
  uintptr_t module_id = image->tls.module_id;
  if (module_id != 0 && module_id <= kMaxTlsModuleSlots)
    __atomic_store_n(&g_tls_module_generations[module_id], uintptr_t{0},
                     __ATOMIC_RELEASE);
  image->tls.active = false;
  image->tls.next = nullptr;
  image->tls.module_id = 0;
  image->tls.generation = 0;
  pthread_mutex_unlock(&g_tls_mutex);
}

extern "C" void *yuki_tls_get_addr(TlsIndex *index) {
  if (index == nullptr)
    return nullptr;
  uintptr_t generation = active_tls_generation(index->module);
  if (generation == 0)
    return nullptr;
  ThreadTlsState *thread = fast_thread_tls_state();
  if (thread != nullptr) {
    ThreadTlsBlock *block = find_thread_block(thread, index->module);
    if (block != nullptr && block->generation == generation)
      return static_cast<uint8_t *>(block->storage) + index->offset;
  }
  if (thread == nullptr)
    thread = thread_tls_state();
  if (thread == nullptr)
    return nullptr;

  // Blocks belong exclusively to the current thread. Once allocated, their
  // storage and generation are stable until the module ID is reused. A reused
  // slot takes the slow path once to replace its stale per-thread block.

  pthread_mutex_lock(&g_tls_mutex);
  TlsTemplate *tls = find_tls_template(index->module);
  if (tls == nullptr || index->offset >= tls->memory_bytes) {
    pthread_mutex_unlock(&g_tls_mutex);
    return nullptr;
  }
  ThreadTlsBlock *block = find_thread_block(thread, tls->module_id);
  if (block == nullptr) {
    if (!ensure_thread_block_capacity(thread, tls->module_id)) {
      pthread_mutex_unlock(&g_tls_mutex);
      return nullptr;
    }
    block = create_thread_block(*tls);
    if (block == nullptr) {
      pthread_mutex_unlock(&g_tls_mutex);
      return nullptr;
    }
    block->next = thread->first;
    thread->first = block;
    thread->blocks[tls->module_id] = block;
  } else if (block->generation != tls->generation &&
             !reset_thread_block(block, *tls)) {
    pthread_mutex_unlock(&g_tls_mutex);
    return nullptr;
  }
  void *address = static_cast<uint8_t *>(block->storage) + index->offset;
  pthread_mutex_unlock(&g_tls_mutex);
  return address;
}

void *current_thread_tls_data(uintptr_t module_id, uintptr_t generation) {
  if (module_id == 0 || generation == 0 ||
      active_tls_generation(module_id) != generation)
    return nullptr;
  ThreadTlsState *thread = fast_thread_tls_state();
  if (thread == nullptr)
    thread =
        static_cast<ThreadTlsState *>(pthread_getspecific(g_thread_tls_key));
  if (thread == nullptr)
    return nullptr;
  ThreadTlsBlock *block = find_thread_block(thread, module_id);
  return block != nullptr && block->generation == generation ? block->storage
                                                             : nullptr;
}

uintptr_t current_thread_pointer() {
  return reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
}

#if defined(__aarch64__)
extern "C" uintptr_t yuki_tlsdesc_dynamic_entry(uintptr_t *descriptor);
extern "C" uintptr_t yuki_tlsdesc_weak_entry(uintptr_t *descriptor);
extern "C" __attribute__((visibility("hidden"))) uintptr_t
yuki_tlsdesc_dynamic_body(uintptr_t *descriptor) {
  auto *index = reinterpret_cast<TlsIndex *>(descriptor[1]);
  void *address = yuki_tls_get_addr(index);
  return reinterpret_cast<uintptr_t>(address) - current_thread_pointer();
}
#endif // #if defined(__aarch64__)
#else
struct TlsIndex {
  unsigned long module;
  unsigned long offset;
};
bool activate_tls(ImageState *) { return true; }
void deactivate_tls(ImageState *) {}
extern "C" void *yuki_tls_get_addr(TlsIndex *) { return nullptr; }
void *current_thread_tls_data(uintptr_t, uintptr_t) { return nullptr; }
#endif // #if YUKILINKER_FULL

bool discover_tls_segment(ImageState *image) {
  bool found = false;
  for (size_t i = 0; i < image->memory.program_header_count; ++i) {
    const ElfW(Phdr) &ph = image->memory.program_headers[i];
    if (ph.p_type != PT_TLS)
      continue;
    if (found || ph.p_filesz > ph.p_memsz ||
        (ph.p_align > 1 && !is_power_of_two(ph.p_align)))
      return false;
    found = true;
    image->tls.initial_bytes =
        runtime_pointer<uint8_t>(image, ph.p_vaddr, ph.p_filesz);
    if (ph.p_filesz != 0 && image->tls.initial_bytes == nullptr)
      return false;
    image->tls.file_bytes = ph.p_filesz;
    image->tls.memory_bytes = ph.p_memsz;
    image->tls.alignment = ph.p_align == 0 ? 1 : ph.p_align;
  }
  return true;
}

LifecycleFunction normalized_lifecycle_function(ImageState *image,
                                                LifecycleFunction function) {
  uintptr_t value = reinterpret_cast<uintptr_t>(function);
  if (value == 0 || image_contains(image, reinterpret_cast<void *>(value)))
    return function;
  auto *rebased = runtime_pointer<uint8_t>(image, value);
  if (rebased != nullptr)
    return reinterpret_cast<LifecycleFunction>(rebased);
  return function;
}

void run_initializers(ImageState *image) {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_exit_mutex);
  g_initializing_image = image;
  pthread_mutex_unlock(&g_exit_mutex);
#endif // #if YUKILINKER_FULL
  LifecycleFunction init =
      normalized_lifecycle_function(image, image->lifecycle.init);
  if (init != nullptr)
    init();
  for (size_t i = 0; i < image->lifecycle.init_count; ++i) {
    LifecycleFunction entry =
        normalized_lifecycle_function(image, image->lifecycle.init_array[i]);
    if (entry != nullptr)
      entry();
  }
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_exit_mutex);
  if (g_initializing_image == image)
    g_initializing_image = nullptr;
  pthread_mutex_unlock(&g_exit_mutex);
#endif // #if YUKILINKER_FULL
  image->lifecycle.initialized = true;
}

void run_finalizers(ImageState *image) {
  if (image == nullptr || !image->lifecycle.initialized)
    return;
  for (size_t i = image->lifecycle.fini_count; i != 0; --i) {
    LifecycleFunction entry = normalized_lifecycle_function(
        image, image->lifecycle.fini_array[i - 1]);
    if (entry != nullptr)
      entry();
  }
  LifecycleFunction fini =
      normalized_lifecycle_function(image, image->lifecycle.fini);
  if (fini != nullptr)
    fini();
#if YUKILINKER_FULL
  drain_exit_callbacks(image, nullptr);
#endif // #if YUKILINKER_FULL
  image->lifecycle.initialized = false;
}

uintptr_t invoke_ifunc(uintptr_t resolver) {
#if defined(__aarch64__)
  using Resolver = ElfW(Addr) (*)(uint64_t);
  return reinterpret_cast<Resolver>(resolver)(getauxval(AT_HWCAP));
#else
  using Resolver = ElfW(Addr) (*)();
  return reinterpret_cast<Resolver>(resolver)();
#endif // #if defined(__aarch64__)
}

struct SymbolResolution {
  uintptr_t address = 0;
  bool valid = false;
};

SymbolResolution materialize_defined_symbol(ImageState *image,
                                            const ElfW(Sym) & symbol) {
  if (symbol.st_shndx == SHN_UNDEF)
    return {};
  uintptr_t address;
  if (symbol.st_shndx == SHN_ABS) {
    address = symbol.st_value;
  } else {
    auto *pointer = runtime_pointer<uint8_t>(image, symbol.st_value);
    if (pointer == nullptr)
      return {};
    address = reinterpret_cast<uintptr_t>(pointer);
  }
  if (ELF64_ST_TYPE(symbol.st_info) == STT_GNU_IFUNC)
    address = invoke_ifunc(address);
  return {address, true};
}

SymbolResolution resolve_symbol(ImageState *image, uint32_t symbol_index) {
  const ElfW(Sym) *symbol = symbol_at(image, symbol_index);
  if (symbol == nullptr || !symbol_name_valid(image->symbols, *symbol))
    return {};
  const char *name = image->symbols.strings + symbol->st_name;

  if (symbol->st_shndx == SHN_UNDEF) {
    if (strcmp(name, "dl_iterate_phdr") == 0)
      return {reinterpret_cast<uintptr_t>(&dl_iterate_phdr_hook), true};
    if (strcmp(name, "__cxa_atexit") == 0)
      return {reinterpret_cast<uintptr_t>(&module_cxa_atexit), true};
    if (strcmp(name, "__cxa_finalize") == 0)
      return {reinterpret_cast<uintptr_t>(&module_cxa_finalize), true};
    if (strcmp(name, "__cxa_thread_atexit_impl") == 0)
      return {reinterpret_cast<uintptr_t>(&yuki_thread_atexit_noop), true};
#if YUKILINKER_FULL
    if (strcmp(name, "__tls_get_addr") == 0)
      return {reinterpret_cast<uintptr_t>(&yuki_tls_get_addr), true};
#endif // #if YUKILINKER_FULL
  } else
    return materialize_defined_symbol(image, *symbol);

  for (Dependency *dependency = image->dependencies; dependency != nullptr;
       dependency = dependency->next) {
    if (dependency->system_handle == nullptr)
      continue;
    void *address = ::dlsym(dependency->system_handle, name);
    if (address != nullptr)
      return {reinterpret_cast<uintptr_t>(address), true};
  }
  void *global = ::dlsym(RTLD_DEFAULT, name);
  if (global != nullptr)
    return {reinterpret_cast<uintptr_t>(global), true};
  if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK)
    return {0, true};
  ZLOGE("yukilinker: unresolved import: %s", name);
  return {};
}

#if YUKILINKER_FULL
struct TlsReference {
  uintptr_t module_id = 0;
  uintptr_t byte_offset = 0;
  uintptr_t generation = 0;
  bool unresolved_weak = false;
};

bool resolve_tls_reference(ImageState *image, uint32_t symbol_index,
                           ElfW(Sxword) addend, TlsReference *result) {
  *result = {};
  if (symbol_index == 0) {
    uintptr_t offset;
    if (!image->tls.active || !add_signed_offset(0, addend, &offset) ||
        offset >= image->tls.memory_bytes)
      return false;
    result->module_id = image->tls.module_id;
    result->byte_offset = offset;
    result->generation = image->tls.generation;
    return true;
  }

  const ElfW(Sym) *symbol = symbol_at(image, symbol_index);
  if (symbol == nullptr)
    return false;
  if (symbol->st_shndx == SHN_UNDEF) {
    if (ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
      return false;
    result->unresolved_weak = true;
    result->byte_offset = static_cast<uintptr_t>(addend);
    return true;
  }
  if (ELF64_ST_TYPE(symbol->st_info) != STT_TLS || !image->tls.active)
    return false;
  uintptr_t offset;
  if (!add_signed_offset(symbol->st_value, addend, &offset) ||
      offset >= image->tls.memory_bytes)
    return false;
  result->module_id = image->tls.module_id;
  result->byte_offset = offset;
  result->generation = image->tls.generation;
  return true;
}
#endif // #if YUKILINKER_FULL

ElfW(Addr) * relocation_destination(ImageState *image, ElfW(Addr) offset) {
  return runtime_pointer<ElfW(Addr)>(image, offset);
}

uint32_t relocation_type(const ElfRelocation &relocation) {
#if defined(__LP64__)
  uint32_t type = ELF64_R_TYPE(relocation.r_info);
  return type;
#else
  return ELF32_R_TYPE(relocation.r_info);
#endif // #if defined(__LP64__)
}

uint32_t relocation_symbol(const ElfRelocation &relocation) {
#if defined(__LP64__)
  return ELF64_R_SYM(relocation.r_info);
#else
  return ELF32_R_SYM(relocation.r_info);
#endif // #if defined(__LP64__)
}

ElfW(Sxword)
    relocation_addend(const ElfRelocation &relocation, ElfW(Addr) value) {
#if defined(__LP64__)
  (void)value;
  return relocation.r_addend;
#else
  (void)relocation;
  return static_cast<ElfW(Sword)>(value);
#endif // #if defined(__LP64__)
}

bool apply_relocation(ImageState *image, const ElfRelocation &relocation) {
  uint32_t type = relocation_type(relocation);
  uint32_t symbol_index = relocation_symbol(relocation);
  ElfW(Addr) *destination = relocation_destination(image, relocation.r_offset);
  if (destination == nullptr)
    return false;
  ElfW(Sxword) addend = relocation_addend(relocation, *destination);

#if defined(__aarch64__)
  switch (type) {
  case R_AARCH64_NONE:
    return true;
  case R_AARCH64_RELATIVE: {
    uintptr_t value;
    if (!add_signed_offset(reinterpret_cast<uintptr_t>(image->memory.bias),
                           addend, &value))
      return false;
    *destination = value;
    return true;
  }
  case R_AARCH64_IRELATIVE: {
    uintptr_t resolver;
    if (!add_signed_offset(reinterpret_cast<uintptr_t>(image->memory.bias),
                           addend, &resolver) ||
        !image_contains(image, reinterpret_cast<void *>(resolver)))
      return false;
    *destination = invoke_ifunc(resolver);
    return true;
  }
#if YUKILINKER_FULL
  case R_AARCH64_TLSDESC: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    auto *descriptor = reinterpret_cast<uintptr_t *>(destination);
    if (reference.unresolved_weak) {
      descriptor[0] = reinterpret_cast<uintptr_t>(&yuki_tlsdesc_weak_entry);
      descriptor[1] = reference.byte_offset;
      return true;
    }
    auto *cookie = metadata_object<TlsDescriptorIndex>();
    if (cookie == nullptr)
      return false;
    cookie->module = reference.module_id;
    cookie->offset = reference.byte_offset;
    cookie->generation = reference.generation;
    descriptor[0] = reinterpret_cast<uintptr_t>(&yuki_tlsdesc_dynamic_entry);
    descriptor[1] = reinterpret_cast<uintptr_t>(cookie);
    return true;
  }
  case R_AARCH64_TLS_DTPREL64: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    *destination = reference.byte_offset;
    return true;
  }
  case R_AARCH64_TLS_DTPMOD64: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    *destination = reference.unresolved_weak ? 0 : reference.module_id;
    return true;
  }
  case R_AARCH64_TLS_TPREL64: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    if (reference.unresolved_weak) {
      *destination = 0;
      return true;
    }
    ZLOGE("yukilinker: static TLS access model is unsupported");
    return false;
  }
#endif // #if YUKILINKER_FULL
  case R_AARCH64_ABS64: {
    SymbolResolution symbol = resolve_symbol(image, symbol_index);
    uintptr_t value;
    if (!symbol.valid || !add_signed_offset(symbol.address, addend, &value))
      return false;
    *destination = value;
    return true;
  }
  case R_AARCH64_GLOB_DAT:
  case R_AARCH64_JUMP_SLOT: {
    SymbolResolution symbol = resolve_symbol(image, symbol_index);
    if (!symbol.valid)
      return false;
    *destination = symbol.address;
    return true;
  }
  default:
    ZLOGE("yukilinker: unsupported AArch64 relocation %u", type);
    return false;
  }
#else
  switch (type) {
  case R_ARM_NONE:
    return true;
  case R_ARM_RELATIVE: {
    uintptr_t value;
    if (!add_signed_offset(reinterpret_cast<uintptr_t>(image->memory.bias),
                           addend, &value))
      return false;
    *destination = static_cast<ElfW(Addr)>(value);
    return true;
  }
  case R_ARM_IRELATIVE: {
    uintptr_t resolver;
    if (!add_signed_offset(reinterpret_cast<uintptr_t>(image->memory.bias),
                           addend, &resolver) ||
        !image_contains(image, reinterpret_cast<void *>(resolver)))
      return false;
    *destination = static_cast<ElfW(Addr)>(invoke_ifunc(resolver));
    return true;
  }
  case R_ARM_ABS32: {
    SymbolResolution symbol = resolve_symbol(image, symbol_index);
    uintptr_t value;
    if (!symbol.valid || !add_signed_offset(symbol.address, addend, &value))
      return false;
    *destination = static_cast<ElfW(Addr)>(value);
    return true;
  }
  case R_ARM_REL32: {
    SymbolResolution symbol = resolve_symbol(image, symbol_index);
    uintptr_t value;
    if (!symbol.valid || !add_signed_offset(symbol.address, addend, &value))
      return false;
    *destination = static_cast<ElfW(Addr)>(
        value - reinterpret_cast<uintptr_t>(destination));
    return true;
  }
  case R_ARM_GLOB_DAT:
  case R_ARM_JUMP_SLOT: {
    SymbolResolution symbol = resolve_symbol(image, symbol_index);
    if (!symbol.valid)
      return false;
    *destination = static_cast<ElfW(Addr)>(symbol.address);
    return true;
  }
#if YUKILINKER_FULL
  case R_ARM_TLS_DTPMOD32: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, 0, &reference))
      return false;
    *destination = static_cast<ElfW(Addr)>(
        reference.unresolved_weak ? 0 : reference.module_id);
    return true;
  }
  case R_ARM_TLS_DTPOFF32: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    *destination = static_cast<ElfW(Addr)>(reference.byte_offset);
    return true;
  }
  case R_ARM_TLS_TPOFF32: {
    TlsReference reference;
    if (!resolve_tls_reference(image, symbol_index, addend, &reference))
      return false;
    if (reference.unresolved_weak) {
      *destination = 0;
      return true;
    }
    ZLOGE("yukilinker: ARM static TLS access model is unsupported");
    return false;
  }
#endif // #if YUKILINKER_FULL
  case R_ARM_TLS_DESC:
    ZLOGE("yukilinker: ARM TLSDESC is not emitted by Android lld");
    return false;
  default:
    ZLOGE("yukilinker: unsupported ARM relocation %u", type);
    return false;
  }
#endif // #if defined(__aarch64__)
}

bool apply_relocation_span(ImageState *image, const ElfRelocation *entries,
                           size_t bytes) {
  if (bytes == 0)
    return true;
  if (entries == nullptr || bytes % sizeof(ElfRelocation) != 0)
    return false;
  size_t count = bytes / sizeof(ElfRelocation);
  for (size_t i = 0; i < count; ++i)
    if (!apply_relocation(image, entries[i]))
      return false;
  return true;
}

bool patch_relative_word(ImageState *image, ElfW(Addr) offset) {
  auto *word = runtime_pointer<ElfW(Addr)>(image, offset);
  if (word == nullptr)
    return false;
  *word += reinterpret_cast<ElfW(Addr)>(image->memory.bias);
  return true;
}

bool apply_relr_span(ImageState *image) {
  const RelocationSet &relocations = image->relocations;
  if (relocations.relr_bytes == 0)
    return true;
  if (relocations.relr == nullptr ||
      relocations.relr_bytes % sizeof(ElfW(Addr)) != 0)
    return false;

  size_t count = relocations.relr_bytes / sizeof(ElfW(Addr));
  ElfW(Addr) next_offset = 0;
  constexpr size_t kBitmapBits = (sizeof(ElfW(Addr)) * 8) - 1;
  for (size_t i = 0; i < count; ++i) {
    ElfW(Addr) encoded = relocations.relr[i];
    if ((encoded & 1U) == 0) {
      if (!patch_relative_word(image, encoded))
        return false;
      if (encoded > UINTPTR_MAX - sizeof(ElfW(Addr)))
        return false;
      next_offset = encoded + sizeof(ElfW(Addr));
      continue;
    }
    ElfW(Addr) bitmap = encoded >> 1;
    for (size_t bit = 0; bit < kBitmapBits; ++bit) {
      size_t displacement = bit * sizeof(ElfW(Addr));
      if ((bitmap & (static_cast<ElfW(Addr)>(1) << bit)) != 0 &&
          (next_offset > UINTPTR_MAX - displacement ||
           !patch_relative_word(image, next_offset + displacement)))
        return false;
    }
    constexpr size_t kBitmapSpan = kBitmapBits * sizeof(ElfW(Addr));
    if (next_offset > UINTPTR_MAX - kBitmapSpan)
      return false;
    next_offset += kBitmapSpan;
  }
  return true;
}

bool relocate_image(ImageState *image) {
  return apply_relr_span(image) &&
         apply_relocation_span(image, image->relocations.rel,
                               image->relocations.rel_bytes) &&
         apply_relocation_span(image, image->relocations.plt,
                               image->relocations.plt_bytes);
}

void release_failed_image(ImageState *image) {
  if (image == nullptr)
    return;
  deactivate_tls(image);
  close_dependencies(image);
  if (image->memory.reservation != nullptr) {
    munmap(image->memory.reservation, image->memory.span);
    image->memory.reservation = nullptr;
  }
  if (image->public_handle != nullptr) {
    image->public_handle->load_bias = nullptr;
    image->public_handle->map_size = 0;
    image->public_handle->private_state = nullptr;
  }
}

} // namespace

SoHandle *dlopen_memfd(int memfd, const char *vma_name, bool file_backed) {
  (void)vma_name;
  struct stat status{};
  if (memfd < 0 || fstat(memfd, &status) != 0 || status.st_size <= 0)
    return nullptr;
  size_t file_size = static_cast<size_t>(status.st_size);
  void *source = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, memfd, 0);
  if (source == MAP_FAILED)
    return nullptr;

  SourceLayout layout;
  if (!inspect_source(source, file_size, &layout)) {
    munmap(source, file_size);
    ZLOGE("yukilinker: invalid shared object for this ABI");
    return nullptr;
  }

  SoHandle *handle = metadata_object<SoHandle>();
  ImageState *image = metadata_object<ImageState>();
  if (handle == nullptr || image == nullptr) {
    munmap(source, file_size);
    return nullptr;
  }
  handle->private_state = image;
  image->public_handle = handle;
  image->display_name = "";

  if (!create_address_space(memfd, static_cast<const uint8_t *>(source),
                            file_size, layout, file_backed, &image->memory)) {
    munmap(source, file_size);
    release_failed_image(image);
    return nullptr;
  }
  handle->load_bias = image->memory.bias;
  handle->map_size = image->memory.span;

  DynamicParse dynamic;
  bool metadata_ready =
      discover_tls_segment(image) &&
      parse_dynamic_tags(image, layout.dynamic_index, &dynamic) &&
      establish_symbol_count(image, dynamic);
  if (metadata_ready && requires_system_tls_runtime(image)) {
    munmap(source, file_size);
    release_failed_image(image);
    return nullptr;
  }
  if (metadata_ready)
    build_symbol_index(image);
  if (!metadata_ready || !open_dependencies(image, dynamic) ||
      !activate_tls(image) || !relocate_image(image) ||
      !restore_anonymous_protections(image) || !protect_relro(image)) {
    munmap(source, file_size);
    release_failed_image(image);
    return nullptr;
  }

  munmap(source, file_size);
  register_image(image);
  run_initializers(image);
  return handle;
}

void *dlsym(SoHandle *handle, const char *name) {
  ImageState *image = state_of(handle);
  const ElfW(Sym) *symbol = find_defined_symbol(image, name);
  if (symbol == nullptr)
    return nullptr;
#if YUKILINKER_FULL
  if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS) {
    if (!image->tls.active)
      return nullptr;
    TlsIndex index{image->tls.module_id, symbol->st_value};
    return yuki_tls_get_addr(&index);
  }
#endif // #if YUKILINKER_FULL
  SymbolResolution resolution = materialize_defined_symbol(image, *symbol);
  return resolution.valid ? reinterpret_cast<void *>(resolution.address)
                          : nullptr;
}

void dlclose(SoHandle *handle) {
  ImageState *image = state_of(handle);
  if (image == nullptr)
    return;
  run_finalizers(image);
  discard_exit_callbacks(image);
  deactivate_tls(image);
  if (image->memory.reservation != nullptr &&
      munmap(image->memory.reservation, image->memory.span) != 0) {
    ZLOGE("yukilinker: image unmap failed");
    return;
  }
  unregister_image(image);
  close_dependencies(image);
  image->memory.reservation = nullptr;
  handle->load_bias = nullptr;
  handle->map_size = 0;
  handle->private_state = nullptr;
}

bool has_active_tls() {
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_tls_mutex);
  bool active = g_tls_templates != nullptr;
  pthread_mutex_unlock(&g_tls_mutex);
  return active;
#else
  return false;
#endif // #if YUKILINKER_FULL
}

bool shutdown() {
  registry_lock();
  const bool images_empty = g_first_image == nullptr;
  registry_unlock();
  if (!images_empty) {
    ZLOGE("yukilinker: shutdown blocked by loaded images");
    return false;
  }
#if YUKILINKER_FULL
#if defined(__aarch64__)
  disable_tls_fast_path();
#endif // #if defined(__aarch64__)
  pthread_mutex_lock(&g_tls_mutex);
  g_tls_stopped = true;
  for (TlsTemplate *tls = g_tls_templates; tls != nullptr; tls = tls->next)
    tls->active = false;
  g_tls_templates = nullptr;
  for (size_t i = 1; i <= kMaxTlsModuleSlots; ++i)
    __atomic_store_n(&g_tls_module_generations[i], uintptr_t{0},
                     __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_tls_mutex);

  if (__atomic_load_n(&g_thread_tls_key_valid, __ATOMIC_ACQUIRE)) {
    auto *thread =
        static_cast<ThreadTlsState *>(pthread_getspecific(g_thread_tls_key));
    pthread_setspecific(g_thread_tls_key, nullptr);
    destroy_thread_tls_state(thread);
    pthread_key_delete(g_thread_tls_key);
    __atomic_store_n(&g_thread_tls_key_valid, false, __ATOMIC_RELEASE);
  }
#endif // #if YUKILINKER_FULL

  MetadataPage *pages = nullptr;
#if YUKILINKER_FULL
  pthread_mutex_lock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
  pages = g_metadata_pages;
  g_metadata_pages = nullptr;
#if YUKILINKER_FULL
  pthread_mutex_unlock(&g_metadata_mutex);
#endif // #if YUKILINKER_FULL
  while (pages != nullptr) {
    MetadataPage *next = pages->next;
    if (munmap(pages, pages->mapped_size) != 0) {
      g_metadata_pages = pages;
      return false;
    }
    pages = next;
  }
  return true;
}

bool release_bootstrap_metadata(SoHandle *handle) {
  ImageState *image = state_of(handle);
  if (image == nullptr || image->tls.active || g_first_image != image ||
      g_last_image != image)
    return false;
  // The core owns its mapping after verifying every borrowed entry point.
  unregister_image(image);
  close_dependencies(image);
  handle->private_state = nullptr;
  return shutdown();
}

extern "C" void __cxa_finalize(void *);
extern "C" __attribute__((visibility("hidden"))) void *__dso_handle;

void finalize_self_dso() { __cxa_finalize(&__dso_handle); }

using SystemIterateFunction = int (*)(int (*)(struct dl_phdr_info *, size_t,
                                              void *),
                                      void *);

SystemIterateFunction resolve_system_iterator() {
  // Build the symbol name at runtime so the compiler cannot introduce a direct
  // import to the function being wrapped.
  constexpr uint8_t encoded[] = {0xdf, 0xd7, 0xe4, 0xd2, 0xcf, 0xde,
                                 0xc9, 0xda, 0xcf, 0xde, 0xe4, 0xcb,
                                 0xd3, 0xdf, 0xc9, 0x00};
  char name[sizeof(encoded)];
  for (size_t i = 0; i + 1 < sizeof(encoded); ++i)
    name[i] = static_cast<char>(encoded[i] ^ 0xbb);
  name[sizeof(encoded) - 1] = '\0';
  return reinterpret_cast<SystemIterateFunction>(::dlsym(RTLD_DEFAULT, name));
}

struct PhdrSnapshot {
  struct dl_phdr_info info;
  uintptr_t tls_generation;
};

int dl_iterate_phdr_hook(int (*callback)(struct dl_phdr_info *, size_t, void *),
                         void *data) {
  if (callback == nullptr)
    return 0;
  static SystemIterateFunction system_iterator = resolve_system_iterator();
  int result = system_iterator == nullptr ? 0 : system_iterator(callback, data);
  if (result != 0)
    return result;

  registry_lock();
  size_t image_count = 0;
  for (ImageState *image = g_first_image; image != nullptr; image = image->next)
    ++image_count;
  size_t snapshot_bytes;
  if (multiply_overflow(image_count, sizeof(PhdrSnapshot), &snapshot_bytes)) {
    registry_unlock();
    return 0;
  }
  auto *snapshot = static_cast<PhdrSnapshot *>(
      snapshot_bytes == 0 ? nullptr : malloc(snapshot_bytes));
  if (snapshot_bytes != 0 && snapshot == nullptr) {
    registry_unlock();
    return 0;
  }
  size_t snapshot_index = 0;
  for (ImageState *image = g_first_image; image != nullptr;
       image = image->next) {
    PhdrSnapshot &entry = snapshot[snapshot_index++];
    struct dl_phdr_info &info = entry.info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = reinterpret_cast<ElfW(Addr)>(image->memory.bias);
    info.dlpi_name = image->display_name;
    info.dlpi_phdr = image->memory.program_headers;
    info.dlpi_phnum =
        static_cast<ElfW(Half)>(image->memory.program_header_count);
    if (image->tls.active) {
      info.dlpi_tls_modid = image->tls.module_id;
      entry.tls_generation = image->tls.generation;
    } else {
      entry.tls_generation = 0;
    }
  }
  registry_unlock();

  for (size_t i = 0; i < image_count; ++i) {
    PhdrSnapshot &entry = snapshot[i];
    entry.info.dlpi_tls_data = current_thread_tls_data(
        entry.info.dlpi_tls_modid, entry.tls_generation);
    result = callback(&entry.info, sizeof(entry.info), data);
    if (result != 0)
      break;
  }
  free(snapshot);
  return result;
}

} // namespace yukilinker

namespace {

void raw_close_descriptor(int fd) {
#if defined(__aarch64__)
  register long syscall_number asm("x8") = 57;
  register long argument asm("x0") = fd;
  asm volatile("svc #0" : "+r"(argument) : "r"(syscall_number) : "memory");
#else
  (void)syscall(__NR_close, fd);
#endif // #if defined(__aarch64__)
}

bool read_exactly(int fd, void *buffer, size_t bytes) {
  auto *cursor = static_cast<uint8_t *>(buffer);
  while (bytes != 0) {
    ssize_t amount = read(fd, cursor, bytes);
    if (amount <= 0)
      return false;
    cursor += amount;
    bytes -= static_cast<size_t>(amount);
  }
  return true;
}

void close_early_packet(int packet_fd) {
  if (packet_fd < 0)
    return;
  yz_early_native_packet_header header{};
  if (lseek(packet_fd, 0, SEEK_SET) == 0 &&
      read_exactly(packet_fd, &header, sizeof(header)) &&
      header.magic == YZ_EARLY_NATIVE_PACKET_MAGIC &&
      header.version == YZ_EARLY_NATIVE_VERSION &&
      header.header_size == sizeof(header) &&
      header.entry_size == sizeof(yz_early_native_packet_entry) &&
      header.count <= YZ_NATIVE_TARGET_MAX) {
    for (uint32_t i = 0; i < header.count; ++i) {
      yz_early_native_packet_entry entry{};
      if (!read_exactly(packet_fd, &entry, sizeof(entry)))
        break;
      if (entry.fd >= 0)
        raw_close_descriptor(entry.fd);
    }
  }
  raw_close_descriptor(packet_fd);
}

#if defined(__LP64__)
constexpr char kCorePath[] = "/data/adb/ksu/lib/yukizygisk/libzygisk64.so";
#else
constexpr char kCorePath[] = "/data/adb/ksu/lib/yukizygisk/libzygisk32.so";
#endif // #if defined(__LP64__)

} // namespace

extern "C" {

#if !defined(YUKILINKER_BOOTSTRAP)
// The core must never retain the first-stage loader's exported entry points.
// These hidden aliases bind directly to the copy of yukilinker compiled into
// the core; the bootstrap-only exports below remain the stage-one ABI.
[[gnu::visibility("hidden")]] void *
yuki_core_dlopen_memfd(int memfd, const char *vma_name) {
  return yukilinker::dlopen_memfd(memfd, vma_name, false);
}

[[gnu::visibility("hidden")]] void *yuki_core_dlsym(void *handle,
                                                    const char *name) {
  return yukilinker::dlsym(static_cast<yukilinker::SoHandle *>(handle), name);
}

[[gnu::visibility("hidden")]] void yuki_core_dlclose(void *handle) {
  yukilinker::dlclose(static_cast<yukilinker::SoHandle *>(handle));
}
#endif // #if !defined(YUKILINKER_BOOTSTRAP)

[[gnu::visibility("default")]] void *yuki_dlopen_memfd(int memfd,
                                                       const char *vma_name) {
  return yukilinker::dlopen_memfd(memfd, vma_name, false);
}

[[gnu::visibility("default")]] void *yuki_dlsym(void *handle,
                                                const char *name) {
  return yukilinker::dlsym(static_cast<yukilinker::SoHandle *>(handle), name);
}

[[gnu::visibility("default")]] void yuki_dlclose(void *handle) {
  yukilinker::dlclose(static_cast<yukilinker::SoHandle *>(handle));
}

[[gnu::visibility("default")]] void yuki_bootstrap(int core_fd,
                                                   int early_packet_fd_plus1) {
  int packet_fd = early_packet_fd_plus1 > 0 ? early_packet_fd_plus1 - 1 : -1;
  if (core_fd < 0) {
    close_early_packet(packet_fd);
    return;
  }

  yukilinker::SoHandle *core = yukilinker::dlopen_memfd(core_fd, "", false);
  raw_close_descriptor(core_fd);
  if (core == nullptr) {
    close_early_packet(packet_fd);
    return;
  }

  using CoreEntry = void (*)(const char *, void *, void *, void *, int);
  auto entry =
      reinterpret_cast<CoreEntry>(yukilinker::dlsym(core, "zygisk_core_entry"));
  if (entry == nullptr) {
    close_early_packet(packet_fd);
    return;
  }
  entry(kCorePath, reinterpret_cast<void *>(&yuki_bootstrap),
        reinterpret_cast<void *>(core->load_bias),
        reinterpret_cast<void *>(core->map_size), early_packet_fd_plus1);
  using FinalizeLoader = void (*)(int, int);
  auto finalize = reinterpret_cast<FinalizeLoader>(
      yukilinker::dlsym(core, "zygisk_finalize_loader"));
  using HandoffComplete = bool (*)();
  auto handoff_complete = reinterpret_cast<HandoffComplete>(
      yukilinker::dlsym(core, "zygisk_bootstrap_handoff_complete"));
  if (finalize != nullptr && handoff_complete != nullptr &&
      handoff_complete() && !yukilinker::release_bootstrap_metadata(core)) {
    ZLOGE("bootstrap metadata cleanup failed; retaining loader");
    return;
  }
  yukilinker::finalize_self_dso();
  if (finalize != nullptr) [[clang::musttail]]
    return finalize(0, 0); // NOLINT(readability-avoid-return-with-void-value)
}

} // extern "C"
