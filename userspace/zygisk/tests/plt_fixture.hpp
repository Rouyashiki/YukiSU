#pragma once

#include "native_bionic/check.hpp"
#include "plt_lifecycle.hpp"
#include "process_maps.hpp"
#include <yukiplt.hpp>

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct PltFixture {
  void *library = nullptr;
  yukizygisk::MapInfo map;
  int (*call)() = nullptr;

  explicit PltFixture(const char *path) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    CHECK(library != nullptr);
    call = reinterpret_cast<int (*)()>(dlsym(library, "fixture_call"));
    CHECK(call != nullptr && call() == 17);
    const auto address = reinterpret_cast<uintptr_t>(call);
    for (const auto &entry : yukizygisk::MapInfo::Scan()) {
      if (entry.start <= address && address < entry.end)
        map = entry;
    }
    CHECK(map.inode != 0);
  }
  ~PltFixture() { CHECK(dlclose(library) == 0); }
  PltFixture(const PltFixture &) = delete;
  PltFixture &operator=(const PltFixture &) = delete;

  yukiplt::NamedImage image() const {
    auto found = yukiplt::find_image_containing(reinterpret_cast<void *>(call));
    CHECK(found);
    return std::move(found.value());
  }
};

inline void write_fixture_slot(uintptr_t address, void *value) {
  const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  void *base = reinterpret_cast<void *>(address & ~(page - 1));
  CHECK(mprotect(base, page, PROT_READ | PROT_WRITE) == 0);
  __atomic_store_n(reinterpret_cast<void **>(address), value, __ATOMIC_RELEASE);
  CHECK(mprotect(base, page, PROT_READ) == 0);
}
