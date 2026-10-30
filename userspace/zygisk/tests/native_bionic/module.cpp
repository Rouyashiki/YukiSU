#include "check.hpp"
#include "zygisk_next_api.h"

#include <dlfcn.h>
#include <link.h>

#include <cstdint>
#include <cstring>

extern "C" void native_identity_probe();

namespace {
void *initial_base;
int (*original)();
int hook_status = ZN_FAILED;

__attribute__((constructor)) void constructed() { fixture_event(1); }
__attribute__((destructor)) void destroyed() { fixture_event(2); }

struct Image {
  uintptr_t address;
  bool found;
};

int find_image(dl_phdr_info *info, size_t, void *opaque) {
  auto &image = *static_cast<Image *>(opaque);
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const auto &ph = info->dlpi_phdr[i];
    const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
    if (ph.p_type == PT_LOAD && image.address >= start &&
        image.address - start < ph.p_memsz) {
      image.found = true;
      return 1;
    }
  }
  return 0;
}

int hooked() {
  native_identity_probe();
  fixture_event(4);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  const auto target = __atomic_load_n(&original, __ATOMIC_ACQUIRE);
  CHECK(target != nullptr);
  return target() + yz_native_private_value();
}

void loaded(void *handle, const ZygiskNextAPI *api) {
  CHECK(handle != nullptr);
  native_identity_probe();
  fixture_event(3);
  hook_status = api->pltHook(nullptr, "yz_native_original",
                             reinterpret_cast<void *>(hooked),
                             reinterpret_cast<void **>(&original));
}
} // namespace

extern "C" void native_identity_probe() {
  Dl_info identity{};
  CHECK(dladdr(reinterpret_cast<void *>(native_identity_probe), &identity));
  CHECK(identity.dli_fbase != nullptr && identity.dli_fname != nullptr);
  if (initial_base == nullptr)
    initial_base = identity.dli_fbase;
  CHECK(identity.dli_fbase == initial_base);
  Image image{reinterpret_cast<uintptr_t>(native_identity_probe), false};
  CHECK(dl_iterate_phdr(find_image, &image) == 1 && image.found);

  dlerror();
  void *symbol = dlsym(RTLD_DEFAULT, "yz_native_private_value");
  CHECK(symbol != nullptr && dlerror() == nullptr);
  CHECK(reinterpret_cast<int (*)()>(symbol)() == yz_native_private_value());
  void *dependency = dlopen("libyz_native_private.so", RTLD_NOW | RTLD_NOLOAD);
  CHECK(dependency != nullptr);
  CHECK(dlsym(dependency, "yz_native_private_value") == symbol);
  CHECK(dlsym(RTLD_NEXT, "yz_native_private_value") == symbol);
  CHECK(dlclose(dependency) == 0);
  fixture_event(5);
}

extern "C" void *native_hook_original() {
  return reinterpret_cast<void *>(original);
}

extern "C" int native_hook_status() { return hook_status; }

extern "C" void *native_hook_callback() {
  return reinterpret_cast<void *>(hooked);
}

extern "C" {
ZygiskNextModule zn_module = {ZYGISK_NEXT_API_VERSION, loaded};
}
