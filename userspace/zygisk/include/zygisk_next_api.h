#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZYGISK_NEXT_API_VERSION 4
#define ZYGISK_NEXT_HYOS_API_VERSION 1

#define ZN_SUCCESS 0
#define ZN_FAILED 1

struct ZnSymbolResolver;
struct ZygiskNextRuntime;

// Keep original storage live; replacements must use this publication protocol.
static inline void *zn_plt_load_original(void *const *original) {
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  return __atomic_load_n(original, __ATOMIC_ACQUIRE);
}

struct ZygiskNextAPI {
  // Unique non-null JUMP_SLOT, one owner. A competing replacement is rejected.
  // Repeating the same image/symbol/handler verifies the live owned slot.
  // Failure may retain recovery state; keep code and original storage alive.
  int (*pltHook)(void *base_addr, const char *symbol, void *hook_handler,
                 void **original);
  int (*inlineHook)(void *target, void *addr, void **original);
  int (*inlineUnhook)(void *target);
  struct ZnSymbolResolver *(*newSymbolResolver)(const char *path,
                                                void *base_addr);
  void (*freeSymbolResolver)(struct ZnSymbolResolver *resolver);
  void *(*getBaseAddress)(struct ZnSymbolResolver *resolver);
  void *(*symbolLookup)(struct ZnSymbolResolver *resolver, const char *name,
                        bool prefix, size_t *size);
  void (*forEachSymbols)(struct ZnSymbolResolver *resolver,
                         bool (*callback)(const char *name, void *addr,
                                          size_t size, void *data),
                         void *data);
  int (*connectCompanion)(void *handle);
  const struct ZygiskNextRuntime *(*getRuntime)(void);
};

struct ZygiskNextModule {
  int target_api_version;
  void (*onModuleLoaded)(void *self_handle, const struct ZygiskNextAPI *api);
};

enum ZygiskNextRuntimeType {
  ZN_RUNTIME_HYOS = 1,
};

struct ZnHyosAppSpecializeArgs {
  const char *process_name;
  const char *package_name;
  const char *se_info;
};

struct ZygiskNextHyosModule {
  int target_api_version;
  void (*onAppSpecialized)(const struct ZnHyosAppSpecializeArgs *args);
};

struct ZygiskNextRuntime {
  enum ZygiskNextRuntimeType type;
  int api_version;
  int (*registerModule)(const void *module);
};

struct ZygiskNextCompanionModule {
  int target_api_version;
  void (*onCompanionLoaded)();
  void (*onModuleConnected)(int fd);
};

extern __attribute__((visibility("default"),
                      unused)) struct ZygiskNextModule zn_module;
extern __attribute__((
    visibility("default"),
    unused)) struct ZygiskNextCompanionModule zn_companion_module;

#ifdef __cplusplus
}
#endif
