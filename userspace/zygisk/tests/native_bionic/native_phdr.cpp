#include "kernel/feature/yukizygisk/native_stub.h"
#include <algorithm>
#include <android/dlext.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <link.h>
#include <memory>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#if defined(__aarch64__)
#include "kernel/feature/yukizygisk/native_blob64.h"
#define RESOLVER yz_native_resolver64
#else
#include "kernel/feature/yukizygisk/native_blob32.h"
#define RESOLVER yz_native_resolver32
#endif

namespace {
constexpr size_t kPhdrOffset = 128 * 1024;

struct Fd {
  const int value;
  explicit Fd(int fd) : value(fd) { assert(value >= 0); }
  ~Fd() { close(value); }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

struct Mapping {
  void *const address;
  const size_t size;
  Mapping(size_t length, int protection)
      : address(mmap(nullptr, length, protection, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0)),
        size(length) {
    assert(address != MAP_FAILED);
  }
  ~Mapping() { assert(munmap(address, size) == 0); }
  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;
};

using Library = std::unique_ptr<void, decltype(&dlclose)>;

Library load_relocated_phdr(const char *path) {
  const Fd input(open(path, O_RDONLY | O_CLOEXEC));
  struct stat status{};
  assert(fstat(input.value, &status) == 0 &&
         status.st_size >= static_cast<off_t>(sizeof(ElfW(Ehdr))) &&
         status.st_size < static_cast<off_t>(kPhdrOffset));
  std::vector<unsigned char> image(static_cast<size_t>(status.st_size));
  assert(pread(input.value, image.data(), image.size(), 0) == status.st_size);
  ElfW(Ehdr) header;
  memcpy(&header, image.data(), sizeof(header));
  assert(header.e_phentsize == sizeof(ElfW(Phdr)) && header.e_phnum &&
         header.e_phnum < 64 && header.e_phoff < image.size() &&
         header.e_phnum * sizeof(ElfW(Phdr)) <= image.size() - header.e_phoff);
  std::vector<ElfW(Phdr)> phdr(header.e_phnum);
  memcpy(phdr.data(), image.data() + header.e_phoff,
         phdr.size() * sizeof(ElfW(Phdr)));
  for (const auto &entry : phdr)
    if (entry.p_type == PT_LOAD)
      assert(entry.p_vaddr + entry.p_memsz <= kPhdrOffset);
  if (std::none_of(phdr.begin(), phdr.end(),
                   [](const auto &entry) { return entry.p_type == PT_PHDR; }))
    phdr.insert(phdr.begin(), ElfW(Phdr){PT_PHDR});
  phdr.emplace_back();
  const size_t table_size = phdr.size() * sizeof(ElfW(Phdr));
  ElfW(Phdr) table{};
  table.p_type = PT_PHDR;
  table.p_flags = PF_R;
  table.p_offset = table.p_vaddr = table.p_paddr = kPhdrOffset;
  table.p_filesz = table.p_memsz = table_size;
  table.p_align = alignof(ElfW(Phdr));
  for (auto &entry : phdr)
    if (entry.p_type == PT_PHDR)
      entry = table;
  table.p_type = PT_LOAD;
  table.p_align = 16384;
  phdr.back() = table;
  header.e_phoff = kPhdrOffset;
  header.e_phnum = static_cast<ElfW(Half)>(phdr.size());
  memcpy(image.data(), &header, sizeof(header));

  // Model a post-link optimizer moving PHDRs into a later, readable PT_LOAD.
  const Fd file(memfd_create("relocated-phdr-fixture", MFD_CLOEXEC));
  assert(ftruncate(file.value, kPhdrOffset + table_size) == 0);
  assert(pwrite(file.value, image.data(), image.size(), 0) == status.st_size);
  assert(pwrite(file.value, phdr.data(), table_size, kPhdrOffset) ==
         static_cast<ssize_t>(table_size));
  android_dlextinfo ext{};
  ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  ext.library_fd = file.value;
  Library library(android_dlopen_ext(path, RTLD_NOW | RTLD_LOCAL, &ext),
                  dlclose);
  if (!library)
    fprintf(stderr, "relocated PHDR fixture: %s\n", dlerror());
  assert(library);
  return library;
}

enum class Fault { None, Unmapped, Overflow, Unaligned, Count, Unreadable };

void check(const char *path, Fault fault) {
  auto library = load_relocated_phdr(path);
  auto probe = reinterpret_cast<int (*)()>(
      dlsym(library.get(), "integration_dependency_value"));
  assert(probe && probe() == 41);
  Dl_info identity{};
  assert(dladdr(reinterpret_cast<void *>(probe), &identity));
  auto *base = static_cast<unsigned char *>(identity.dli_fbase);
  ElfW(Ehdr) original;
  memcpy(&original, base, sizeof(original));
  assert(original.e_phoff == kPhdrOffset);
  auto header = original;
  const size_t page = getpagesize();
  const Mapping guard(page, PROT_NONE);
  switch (fault) {
  case Fault::Unmapped:
    header.e_phoff = reinterpret_cast<uintptr_t>(guard.address) -
                     reinterpret_cast<uintptr_t>(base);
    break;
  case Fault::Overflow:
    header.e_phoff =
        std::numeric_limits<ElfW(Off)>::max() - (alignof(ElfW(Phdr)) - 1);
    break;
  case Fault::Unaligned:
    ++header.e_phoff;
    break;
  case Fault::Count:
    header.e_phnum = 129;
    break;
  default:
    break;
  }
  assert(mprotect(base, page, PROT_READ | PROT_WRITE) == 0);
  memcpy(base, &header, sizeof(header));
  assert(mprotect(base, page, PROT_READ) == 0);
  if (fault == Fault::Unreadable)
    assert(mprotect(base + kPhdrOffset, page, PROT_NONE) == 0);

  const size_t length = (sizeof(RESOLVER) + page - 1) & ~(page - 1);
  const Mapping code(length, PROT_READ | PROT_WRITE);
  memcpy(code.address, RESOLVER, sizeof(RESOLVER));
  assert(mprotect(code.address, length, PROT_READ | PROT_EXEC) == 0);
  __builtin___clear_cache(static_cast<char *>(code.address),
                          static_cast<char *>(code.address) + length);
  const Fd empty(open("/dev/null", O_RDONLY | O_CLOEXEC));
  yz_native_stub data{};
  data.magic = YZ_NATIVE_STUB_MAGIC;
  data.version = YZ_NATIVE_STUB_VERSION;
  data.linker = getauxval(AT_BASE);
  data.process = getpid();
  data.generation = 1;
  data.loader_fd = empty.value;
  data.core_fd = data.report_fd = -1;
  reinterpret_cast<void (*)(yz_native_stub *)>(code.address)(&data);

  // Restore the live image before Bionic traverses its PHDRs during dlclose.
  assert(mprotect(base + kPhdrOffset, page, PROT_READ) == 0);
  assert(mprotect(base, page, PROT_READ | PROT_WRITE) == 0);
  memcpy(base, &original, sizeof(original));
  assert(mprotect(base, page, PROT_READ) == 0);
  fprintf(stderr, "relocated PHDR: fault=%u status=%d stage=%u\n",
          static_cast<unsigned>(fault), data.status, data.stage);
  // An empty loader fd stops the successful census before loading SDK/Core.
  const int expected_status = fault == Fault::None ? 6 : 4;
  const unsigned expected_stage = fault == Fault::None         ? 0
                                  : fault == Fault::Unreadable ? 101
                                                               : 102;
  assert(data.status == expected_status && data.stage == expected_stage &&
         !data.loader && !data.bootstrap && !data.libc_reference);
  assert(probe() == 41);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  for (auto fault : {Fault::None, Fault::Unmapped, Fault::Overflow,
                     Fault::Unaligned, Fault::Count, Fault::Unreadable})
    check(argv[1], fault);
  puts("PASS generated native resolver accepts relocated PHDRs and rejects "
       "invalid ranges");
}
