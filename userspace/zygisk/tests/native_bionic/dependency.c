#if HOST_DEPENDENCY
#include <dlfcn.h>
#ifndef HOST_VALUE
#define HOST_VALUE 73
#endif
int integration_system_probe(void) { return HOST_VALUE; }
int integration_scope_probe(void) {
  int (*probe)(void) =
      (int (*)(void))dlsym(RTLD_DEFAULT, "integration_system_probe");
  return probe ? probe() : -1;
}
__attribute__((noinline, disable_tail_calls)) void
integration_invoke(void (*callback)(void *), void *context) {
  callback(context);
}
#elif DUPLICATE_LIBC
int getpid(void) { return -1; }
#else
int integration_dependency_value(void) { return 41; }
#endif
