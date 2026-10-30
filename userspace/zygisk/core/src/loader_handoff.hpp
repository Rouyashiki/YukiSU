#pragma once

#include <cstddef>
#include <cstdint>
#include <dlfcn.h>

namespace yukizygisk::handoff {

struct Result {
  bool complete = false;
  const char *reason = "not checked";
  uintptr_t slot = 0;
  uintptr_t target = 0;
  bool tls_retained = false;
};

Result verify(uintptr_t core, size_t size, uintptr_t loader,
              const uintptr_t *capabilities = nullptr, size_t count = 0);
Result verify_loaded(uintptr_t core_reference, uintptr_t loader,
                     const uintptr_t *capabilities = nullptr, size_t count = 0);

using TakeDependencies = bool (*)(void *const *, size_t);
using FinishHandoff = bool (*)(bool);
bool retirement_ready();

// Bootstrap callers already own the initial dlopen reference. This lookup
// validates its identity and balances only the additional NOLOAD reference.
inline void *bootstrap_reference(const void *local_anchor, const char *entry,
                                 const char *soname = nullptr) {
  Dl_info self{};
  if (dladdr(local_anchor, &self) == 0 || self.dli_fname == nullptr)
    return nullptr;
  const char *candidates[] = {self.dli_fname, soname};
  for (const char *name : candidates) {
    if (name == nullptr)
      continue;
    void *handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr)
      continue;
    Dl_info found{};
    const void *symbol = dlsym(handle, entry);
    const bool matched = symbol != nullptr && dladdr(symbol, &found) != 0 &&
                         found.dli_fbase == self.dli_fbase;
    if (dlclose(handle) != 0)
      return nullptr;
    if (matched)
      return handle;
  }
  return nullptr;
}

bool adopt_anonymous_loader(uintptr_t base, size_t span, void *loader);
bool anonymous_image_matches(uintptr_t base, size_t span);
bool release_dependencies();
bool owns_core_reference();
void *take_core_reference();
bool close_loader_reference();

} // namespace yukizygisk::handoff

extern "C" bool zygisk_adopt_bootstrap_handles(void *core, void *loader);
