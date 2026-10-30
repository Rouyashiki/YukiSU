#include "check.hpp"
#include "native_bionic.hpp"
#include "soinfo_layout.hpp"

#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <cstring>
#include <memory>

#include "caller_linker_under_test.inc"
#include "layout_runtime_under_test.inc"

namespace {
volatile unsigned char main_padding[0x260000];
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  main_padding[0] = 1;
  main_padding[sizeof(main_padding) - 1] = 2;
  CHECK(yuki::solist::u_init());
  const auto maps = yukizygisk::MapInfo::Scan();
  yuki::solist::layout::Image executable;
  CHECK(yuki::solist::layout::main_image({maps}, getauxval(AT_PHDR),
                                         getauxval(AT_PHNUM), getpagesize(),
                                         executable));
  CHECK(executable.size > 0x100000);
  void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(handle != nullptr);
  auto answer =
      reinterpret_cast<int (*)()>(dlsym(handle, "caller_anchor_answer"));
  CHECK(answer != nullptr && answer() == 42);
  const uintptr_t address = reinterpret_cast<uintptr_t>(answer);
  const bool keep = strcmp(argv[2], "keep") == 0;
  CHECK(yuki::solist::drop_lib_containing(address, keep) == 1);
  Dl_info identity{};
  CHECK(dladdr(reinterpret_cast<void *>(address), &identity) == 0);
  const auto after = yukizygisk::MapInfo::Scan();
  CHECK(yuki::solist::layout::Memory{after}.covers(address & ~uintptr_t{1},
                                                   1) == keep);
  if (keep)
    CHECK(answer() == 42);
  (void)printf(
      "Bionic soinfo: %zu-byte executable; production unload keep=%d passed\n",
      executable.size, keep);
}
