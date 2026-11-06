#include "check.hpp"
#include "native_bionic.hpp"

#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

using namespace yukizygisk::native_bionic;

static_assert(sizeof(Map{}.inode) >=
              sizeof(std::declval<struct stat>().st_ino));
static_assert(sizeof(Map{}.offset) == sizeof(uint64_t));

namespace {
Maps real_maps() { return read_maps(); }

struct Image {
  int fd = -1;
  char directory[64] = "/tmp/yz-caller-XXXXXX";
  char path[96]{};
  void *memory = MAP_FAILED;
  uintptr_t base = 0;
  size_t size = 0;
  ElfW(Ehdr) *eh = nullptr;
  ElfW(Phdr) *ph = nullptr;
  Identity identity;

  explicit Image(bool symbols) {
    const size_t page = sysconf(_SC_PAGESIZE);
    size = page * 2;
    CHECK(mkdtemp(directory) != nullptr);
    CHECK(snprintf(path, sizeof(path), "%s/%s", directory,
                   symbols ? "linker" : "libc.so") > 0);
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && ftruncate(fd, size) == 0);
    memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
                  fd, 0);
    CHECK(memory != MAP_FAILED);
    initialize(symbols);
  }
  void initialize(bool symbols) {
    const size_t page = size / 2;
    base = reinterpret_cast<uintptr_t>(memory);
    eh = static_cast<ElfW(Ehdr) *>(memory);
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = sizeof(void *) == 8 ? ELFCLASS64 : ELFCLASS32;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_version = EV_CURRENT;
    eh->e_machine = sizeof(void *) == 8 ? EM_AARCH64 : EM_ARM;
    eh->e_type = ET_DYN;
    eh->e_ehsize = sizeof(*eh);
    eh->e_phoff = sizeof(*eh);
    eh->e_phnum = 2;
    eh->e_phentsize = sizeof(ElfW(Phdr));
    ph = reinterpret_cast<ElfW(Phdr) *>(base + eh->e_phoff);
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R;
    ph[0].p_vaddr = page * 2;
    ph[0].p_filesz = ph[0].p_memsz = page;
    ph[0].p_align = page;
    ph[1] = ph[0];
    ph[1].p_flags = PF_R | PF_X;
    ph[1].p_vaddr += page;
    ph[1].p_offset = page;
    if (symbols) {
      eh->e_shoff = 512;
      eh->e_shnum = 3;
      eh->e_shentsize = sizeof(ElfW(Shdr));
      auto *sh = reinterpret_cast<ElfW(Shdr) *>(base + eh->e_shoff);
      sh[1].sh_type = SHT_DYNSYM;
      sh[1].sh_offset = 1024;
      sh[1].sh_size = 6 * sizeof(ElfW(Sym));
      sh[1].sh_entsize = sizeof(ElfW(Sym));
      sh[1].sh_link = 2;
      sh[2].sh_type = SHT_STRTAB;
      sh[2].sh_offset = 2048;
      char *names = reinterpret_cast<char *>(base + sh[2].sh_offset);
      auto *sym = reinterpret_cast<ElfW(Sym) *>(base + sh[1].sh_offset);
      size_t cursor = 1;
      const char *exports[] = {"__loader_android_dlopen_ext",
                               "__loader_dl_iterate_phdr", "__loader_dladdr",
                               "__loader_dlsym", "__loader_dlvsym"};
      for (size_t i = 0; i < 5; ++i) {
        sym[i + 1].st_name = cursor;
        strcpy(names + cursor, exports[i]);
        cursor += strlen(exports[i]) + 1;
        sym[i + 1].st_value = ph[1].p_vaddr + i * 4;
        sym[i + 1].st_size = 4;
        sym[i + 1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        sym[i + 1].st_shndx = 1;
      }
      sh[2].sh_size = cursor;
    }
    struct stat st{};
    CHECK(fstat(fd, &st) == 0);
    identity = {static_cast<uint64_t>(st.st_dev),
                static_cast<uint64_t>(st.st_ino)};
  }
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&) = delete;
  Image &operator=(Image &&) = delete;
  ~Image() {
    munmap(memory, size);
    close(fd);
    unlink(path);
    rmdir(directory);
  }
};

Image *linker_image;
Image *libc_image;
alignas(ElfW(Phdr)) char executable_image[4096];
Image *foreign_libc;
uintptr_t alias_base;
bool include_alias;
bool select_alias;
bool wrong_provider;
pid_t (*fixture_getpid)();
bool duplicate;
bool omit_libc;
bool wrong_dladdr;
bool resolver_error;
bool corrupt_after_find;
bool loader_error;
int internal_calls;
int wrapper_calls;
int expected_fd;
uintptr_t observed_caller;

unsigned long fixture_auxval(unsigned long key) {
  if (key == AT_BASE)
    return linker_image->base;
  if (key == AT_PHDR)
    return reinterpret_cast<uintptr_t>(executable_image);
  if (key == AT_PHNUM)
    return 3;
  if (key == AT_PHENT)
    return sizeof(ElfW(Phdr));
  return getauxval(key);
}

void initialize_executable() {
  memset(executable_image, 0, sizeof(executable_image));
  auto *ph = reinterpret_cast<ElfW(Phdr) *>(executable_image);
  ph[0].p_type = PT_PHDR;
  ph[1].p_type = PT_LOAD;
  ph[1].p_memsz = sizeof(executable_image);
  ph[2].p_type = PT_INTERP;
  ph[2].p_vaddr = 3 * sizeof(ElfW(Phdr));
  ph[2].p_filesz = strlen(linker_image->path) + 1;
  strcpy(executable_image + ph[2].p_vaddr, linker_image->path);
}
} // namespace

#define getauxval fixture_auxval
#if !defined(__arm__) && !defined(__aarch64__)
#define __aarch64__ 1
#define FIXTURE_HOST_AARCH64
#endif
#include "caller_linker_under_test.inc"
#undef getauxval

namespace {
int fixture_iterate(int (*callback)(dl_phdr_info *, size_t, void *),
                    void *data) {
  if (omit_libc)
    return 0;
  dl_phdr_info unrelated{};
  unrelated.dlpi_name = foreign_libc->path;
  CHECK(callback(&unrelated, sizeof(unrelated), data) == 0);
  if (include_alias) {
    dl_phdr_info alias{};
    alias.dlpi_name = libc_image->path;
    alias.dlpi_addr = alias_base - libc_image->ph[0].p_vaddr;
    alias.dlpi_phdr = reinterpret_cast<const ElfW(Phdr) *>(
        alias_base + libc_image->eh->e_phoff);
    alias.dlpi_phnum = libc_image->eh->e_phnum;
    if (callback(&alias, sizeof(alias), data) != 0)
      return 1;
  }
  dl_phdr_info info{};
  info.dlpi_name = libc_image->path;
  info.dlpi_addr = libc_image->base - libc_image->ph[0].p_vaddr;
  info.dlpi_phdr = libc_image->ph;
  info.dlpi_phnum = libc_image->eh->e_phnum;
  const int result = callback(&info, sizeof(info), data);
  return result == 0 && duplicate ? callback(&info, sizeof(info), data)
                                  : result;
}
int fixture_dladdr(const void *address, Dl_info *out) {
  const uintptr_t base = select_alias ? alias_base : libc_image->base;
  const uintptr_t code = code_address(reinterpret_cast<uintptr_t>(address));
  CHECK(code == base || code == base + (libc_image->size / 2));
  out->dli_fbase = reinterpret_cast<void *>(base + (wrong_dladdr ? 1 : 0));
  out->dli_fname = wrong_provider ? foreign_libc->path : libc_image->path;
  if (corrupt_after_find && code == libc_image->base)
    CHECK(mprotect(libc_image->memory, libc_image->size / 2, PROT_NONE) == 0);
  return 1;
}
void *fixture_open(const char *name, int flags, const android_dlextinfo *ext,
                   const void *caller) {
  ++internal_calls;
  CHECK(strcmp(name, "/logical/module.so") == 0);
  CHECK(flags == (RTLD_NOW | RTLD_LOCAL));
  CHECK(ext->flags == ANDROID_DLEXT_USE_LIBRARY_FD);
  CHECK(ext->library_fd == expected_fd && fcntl(expected_fd, F_GETFD) >= 0);
  observed_caller = reinterpret_cast<uintptr_t>(caller);
  if (loader_error) {
    dlopen("/yz-deliberately-absent-caller-test.so", RTLD_NOW);
    errno = ENOENT;
    return nullptr;
  }
  return reinterpret_cast<void *>(1);
}
void *fixture_wrapper(const char *path, int flags,
                      const android_dlextinfo *ext) {
  ++wrapper_calls;
  const auto before = internal_calls;
  auto *result = fixture_open(path, flags, ext, nullptr);
  internal_calls = before;
  return result;
}
void *fixture_lookup(void *, const char *, const void *) { return nullptr; }
void *fixture_lookup_version(void *, const char *, const char *, const void *) {
  return nullptr;
}
LoaderApi mock_api() {
  LoaderApi api;
  api.open = fixture_open;
  api.iterate = fixture_iterate;
  api.address_info = fixture_dladdr;
  api.lookup = fixture_lookup;
  api.lookup_version = fixture_lookup_version;
  for (const auto &map : real_maps())
    if (map.start <= reinterpret_cast<uintptr_t>(fixture_open) &&
        map.end > reinterpret_cast<uintptr_t>(fixture_open))
      api.linker.identity = {static_cast<uint64_t>(map.dev),
                             static_cast<uint64_t>(map.inode)};
  size_t index = 0;
  for (const auto entry :
       {reinterpret_cast<uintptr_t>(fixture_open),
        reinterpret_cast<uintptr_t>(fixture_iterate),
        reinterpret_cast<uintptr_t>(fixture_dladdr),
        reinterpret_cast<uintptr_t>(fixture_lookup),
        reinterpret_cast<uintptr_t>(fixture_lookup_version)}) {
    for (const auto &map : real_maps())
      if (map.start <= code_address(entry) && map.end > code_address(entry))
        api.file_offsets[index] = map.offset + code_address(entry) - map.start;
    ++index;
  }
  CHECK(api.linker.identity.inode != 0);
  return api;
}
} // namespace

bool yukizygisk::native_bionic::resolve_loader(const Maps &, LoaderApi &api,
                                               Failure &failure) {
  if (resolver_error) {
    failure = {Stage::Linker, Error::SymbolUnavailable, 0};
    return false;
  }
  api = mock_api();
  return true;
}

#define getpid fixture_getpid
#define android_dlopen_ext fixture_wrapper
#include "caller_native_under_test.inc"
#undef android_dlopen_ext
#undef getpid
#ifdef FIXTURE_HOST_AARCH64
#undef __aarch64__
#endif

int main() {
  Map wide_identity;
  wide_identity.dev = 1;
  wide_identity.inode = UINT64_C(0x100000002);
  CHECK((Identity{1, UINT64_C(0x100000002)}.matches(wide_identity)));
  CHECK((!Identity{1, 2}.matches(wide_identity)));
  Image linker(true), libc(false), foreign(false);
  linker_image = &linker;
  libc_image = &libc;
  foreign_libc = &foreign;
  void *alias =
      mmap(nullptr, libc.size, PROT_READ | PROT_EXEC, MAP_PRIVATE, libc.fd, 0);
  CHECK(alias != MAP_FAILED);
  alias_base = reinterpret_cast<uintptr_t>(alias);
  fixture_getpid = reinterpret_cast<pid_t (*)()>(libc.base + (libc.size / 2));
  expected_fd = libc.fd;
  initialize_executable();
  auto maps = real_maps();
  LoaderApi api;
  Failure failure;
  CHECK(checked_resolve(maps, api, failure));
  CHECK(reinterpret_cast<uintptr_t>(api.open) ==
        linker.base + sysconf(_SC_PAGESIZE));
  CHECK(api.linker.bias != api.linker.base);
  auto *symbols = reinterpret_cast<ElfW(Sym) *>(linker.base + 1024);
  for (size_t i : {4U, 5U}) {
    const auto symbol = symbols[i];
    symbols[i].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_GNU_IFUNC);
    CHECK(!checked_resolve(maps, api, failure));
    symbols[i] = symbol;
  }
  const auto saved = symbols[1];
#if defined(__arm__)
  symbols[1].st_value |= 1;
  CHECK(checked_resolve(maps, api, failure));
  CHECK((reinterpret_cast<uintptr_t>(api.open) & 1) != 0);
  CHECK(api.file_offsets[0] == linker.size / 2);
  symbols[1] = saved;
  symbols[1].st_value += 2;
  CHECK(!checked_resolve(maps, api, failure));
  symbols[1] = saved;
#endif
  symbols[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_GNU_IFUNC);
  CHECK(!checked_resolve(maps, api, failure));
  symbols[1] = saved;
  symbols[1].st_name += strlen("__loader_");
  CHECK(!checked_resolve(maps, api, failure) &&
        failure.error == Error::SymbolUnavailable);
  symbols[1] = saved;
  symbols[1].st_name = UINT32_MAX;
  CHECK(!checked_resolve(maps, api, failure));
  symbols[1] = saved;
  symbols[1].st_value = UINTPTR_MAX;
  CHECK(!checked_resolve(maps, api, failure));
  symbols[1] = saved;
  symbols[2] = symbols[1];
  CHECK(!checked_resolve(maps, api, failure) &&
        failure.error == Error::SymbolAmbiguous);
  symbols[2] = saved;
  symbols[2].st_name =
      saved.st_name + strlen("__loader_android_dlopen_ext") + 1;
  symbols[2].st_value += 4;
  symbols[1].st_value = linker.ph[0].p_vaddr;
  CHECK(!checked_resolve(maps, api, failure));
  symbols[1] = saved;
  const auto machine = linker.eh->e_machine;
  linker.eh->e_machine = EM_NONE;
  CHECK(!checked_resolve(maps, api, failure));
  linker.eh->e_machine = machine;
  const auto table = linker.eh->e_shoff;
  linker.eh->e_shoff = linker.size - 1;
  CHECK(!checked_resolve(maps, api, failure));
  linker.eh->e_shoff = table;
  for (auto &map : maps)
    if (map.start <= linker.base && map.end > linker.base)
      ++map.inode;
  CHECK(!checked_resolve(maps, api, failure) &&
        failure.error == Error::IdentityMismatch);
  CHECK(!checked_resolve({}, api, failure));
  CHECK(checked_resolve(real_maps(), api, failure));
  maps = real_maps();
  for (auto &map : maps)
    if (map.start <= linker.base && map.end > linker.base)
      map.perms &= ~PROT_EXEC;
  CHECK(!checked_resolve(maps, api, failure));

  android_dlextinfo ext{};
  ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  ext.library_fd = libc.fd;
  const auto run = [&] {
    return load("/logical/module.so", RTLD_NOW | RTLD_LOCAL, ext);
  };
  auto result = run();
  CHECK(result.handle != nullptr && internal_calls == 1 && wrapper_calls == 0);
  CHECK(observed_caller == libc.base);
  CHECK(fcntl(libc.fd, F_GETFD) >= 0);
  include_alias = true;
  CHECK(run().handle != nullptr && observed_caller == libc.base);
  select_alias = true;
  fixture_getpid = reinterpret_cast<pid_t (*)()>(alias_base + (libc.size / 2));
  CHECK(run().handle != nullptr && observed_caller == alias_base);
  select_alias = false;
  include_alias = false;
  fixture_getpid = reinterpret_cast<pid_t (*)()>(libc.base + (libc.size / 2));
#if defined(__arm__)
  fixture_getpid =
      reinterpret_cast<pid_t (*)()>((libc.base + (libc.size / 2)) | 1);
  CHECK(run().handle != nullptr && observed_caller == libc.base);
#endif
  const int successful_calls = internal_calls;
  duplicate = true;
  CHECK(run().failure.error == Error::AnchorAmbiguous);
  duplicate = false;
  omit_libc = true;
  CHECK(run().failure.error == Error::AnchorUnavailable);
  omit_libc = false;
  wrong_dladdr = true;
  CHECK(run().failure.error == Error::IdentityMismatch);
  wrong_dladdr = false;
  corrupt_after_find = true;
  CHECK(run().failure.stage == Stage::Revalidate);
  CHECK(mprotect(libc.memory, libc.size / 2,
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
  corrupt_after_find = false;
  resolver_error = true;
  CHECK(run().failure.error == Error::SymbolUnavailable);
  resolver_error = false;
  auto old_size = libc.ph[0].p_filesz;
  libc.ph[0].p_filesz = UINTPTR_MAX;
  CHECK(run().failure.error == Error::InvalidElf);
  libc.ph[0].p_filesz = old_size;
  CallerContext context;
  maps = real_maps();
  for (auto &map : maps)
    if (map.start <= libc.base && map.end > libc.base)
      map.perms &= ~PROT_READ;
  CHECK(!find_caller(mock_api(), maps,
                     reinterpret_cast<const void *>(fixture_getpid), context,
                     failure));
  CHECK(find_caller(mock_api(), real_maps(),
                    reinterpret_cast<const void *>(fixture_getpid), context,
                    failure));
  ++context.api.file_offsets[0];
  CHECK(!validate_context(context, real_maps(), failure));
  CHECK(internal_calls == successful_calls && wrapper_calls == 0);
  loader_error = true;
  result = run();
  CHECK(result.failure.error == Error::LoaderFailure &&
        result.failure.system_error == ENOENT);
  dlerror();
  CHECK(result.loader_error.find("yz-deliberately-absent") !=
        std::string::npos);
  CHECK(wrapper_calls == 0);
  loader_error = false;
  wrong_provider = true;
  CHECK(run().failure.error == Error::IdentityMismatch && wrapper_calls == 0);
  wrong_provider = false;
  uintptr_t address = 0;
  CHECK(!add_address(UINTPTR_MAX, 1, address));
  CHECK(!mapped_range(real_maps(), UINTPTR_MAX, 4, PROT_READ));
  CHECK(munmap(alias, libc.size) == 0);
  puts("PASS: checked linker, nonzero-bias anchor, bound libc instance, four "
       "arguments, no fallback, revalidation and borrowed FD");
}
