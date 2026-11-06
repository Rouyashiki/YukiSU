#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "handoff_control.hpp"
#include "zygisk.hpp"
#include "zygisk_next_api.h"
#include <assert.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
static IntegrationControl *control;
static __thread int value = 41;
#if NATIVE_IFUNC
static unsigned resolver_calls;
static int resolved(void) { return 79; }
__attribute__((visibility("hidden"))) void *select_implementation(void) {
  ++resolver_calls;
  return (void *)resolved;
}
int module_ifunc(void) __attribute__((ifunc("select_implementation")));
#endif
int module_probe(void);
extern void *__dso_handle;
extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);

__attribute__((constructor)) static void initialize(void) {
  const char *pointer = getenv("YUKISU_INTEGRATION_CONTROL");
  if (pointer) {
    control = (struct IntegrationControl *)(uintptr_t)strtoull(pointer, 0, 16);
    ++control->module_constructors;
#if NATIVE_IFUNC
    assert(module_ifunc() == 79 && resolver_calls == 1);
    control->native_ifunc = resolver_calls;
#endif
#if PRIVATE_DEPENDENCY
    control->caller_bridge = (uintptr_t)dl_iterate_phdr;
    if (control->native_policy)
      assert(module_probe());
#endif
  }
}
__attribute__((destructor)) static void finalize(void) {
  if (control)
    ++control->module_finalizers;
}
static int visit(struct dl_phdr_info *info, size_t size, void *opaque) {
  (void)size;
  if (info->dlpi_name && strstr(info->dlpi_name, "integration_private"))
    ++*(int *)opaque;
  return 0;
}
int module_probe(void) {
#if NATIVE_IFUNC
  assert(module_ifunc() == 79 && resolver_calls == 1);
#endif
#if PRIVATE_DEPENDENCY
  extern int integration_dependency_value(void);
  assert(integration_dependency_value() == 41);
  int (*host_probe)(void) =
      (int (*)(void))dlsym(RTLD_DEFAULT, "integration_system_probe");
  int (*versioned_probe)(void) = (int (*)(void))dlvsym(
      RTLD_DEFAULT, "integration_system_probe", "INTEGRATION_HOST");
  assert(host_probe && host_probe == versioned_probe && host_probe() == 73);
#endif
  int found = 0;
  Dl_info info = {0};
  dl_iterate_phdr(visit, &found);
  if (control && control->native_policy) {
    int (*next_pid)(void) = (int (*)(void))dlsym(RTLD_NEXT, "getpid");
    assert(next_pid && next_pid() == getpid());
  }
  return found == 1 && dladdr((void *)module_probe, &info) && info.dli_fbase;
}
int module_pid(void) { return getpid(); }
int module_open(const char *name) {
  void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  if (!handle)
    return 0;
  assert(dlclose(handle) == 0);
  return 1;
}
int module_fork(void) {
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child)
    _exit(module_probe() ? 0 : 3);
  int status = 0;
  return waitpid(child, &status, 0) == child ? status : -2;
}
static void thread_destroy(void *count) {
  __atomic_fetch_add((unsigned *)count, 1U, __ATOMIC_RELAXED);
}
void *module_thread(void *opaque) {
  assert(value == 41 && ++value == 42);
  assert(__cxa_thread_atexit_impl(thread_destroy, &control->thread_destructors,
                                  &__dso_handle) == 0);
  unsigned *run = (unsigned *)opaque;
  __atomic_store_n(&control->worker_ready, 1U, __ATOMIC_RELEASE);
  while (__atomic_load_n(run, __ATOMIC_ACQUIRE))
    assert(module_probe());
  return 0;
}

#if !MISSING_ENTRY
static void loaded(void *self, const struct ZygiskNextAPI *api) {
  assert(self && api && api->getRuntime);
  if (control) {
    ++control->native_entries;
    control->module_probe = module_probe;
    control->native_thread = module_thread;
    control->native_fork = module_fork;
  }
}
#if INVALID_VERSION
struct ZygiskNextModule zn_module = {999, loaded};
#elif NULL_ENTRY
struct ZygiskNextModule zn_module = {ZYGISK_NEXT_API_VERSION, 0};
#else
struct ZygiskNextModule zn_module = {ZYGISK_NEXT_API_VERSION, loaded};
#endif
#endif
} // extern "C"

#if !MISSING_ENTRY
namespace {
zygisk::internal::api_table *api;
zygisk::internal::module_abi abi(nullptr);
void app_pre(zygisk::ModuleBase *, zygisk::AppSpecializeArgs *) {
  ++control->app_pre;
}
void app_post(zygisk::ModuleBase *, const zygisk::AppSpecializeArgs *) {
  ++control->app_post;
}
void server_pre(zygisk::ModuleBase *, zygisk::ServerSpecializeArgs *) {
  ++control->server_pre;
}
void server_post(zygisk::ModuleBase *, const zygisk::ServerSpecializeArgs *) {
  ++control->server_post;
  api->setOption(api->impl, zygisk::DLCLOSE_MODULE_LIBRARY);
}
} // namespace
extern "C" void zygisk_module_entry(zygisk::internal::api_table *table,
                                    JNIEnv *) {
  control = reinterpret_cast<IntegrationControl *>(
      strtoull(getenv("YUKISU_INTEGRATION_CONTROL"), nullptr, 16));
  api = table;
  abi.api_version = control->reject_registration ? 999 : ZYGISK_API_VERSION;
  abi.impl = reinterpret_cast<zygisk::ModuleBase *>(control);
  abi.preAppSpecialize = app_pre;
  abi.postAppSpecialize = app_post;
  abi.preServerSpecialize = server_pre;
  abi.postServerSpecialize = server_post;
  assert(table->registerModule(table, &abi) == !control->reject_registration);
  ++control->ordinary_entries;
}
#endif
