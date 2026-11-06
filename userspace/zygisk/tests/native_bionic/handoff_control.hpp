#pragma once
#include <stddef.h>
#include <stdint.h>

struct IntegrationControl {
  unsigned adapter_only;
  unsigned module_constructors;
  unsigned module_finalizers;
  unsigned native_entries;
  unsigned ordinary_entries;
  unsigned app_pre;
  unsigned app_post;
  unsigned server_pre;
  unsigned server_post;
  unsigned reject_registration;
  unsigned worker_ready;
  unsigned thread_destructors;
  unsigned native_policy;
  unsigned native_ifunc;
  int (*exercise_exit_lifecycle)(const char *);
  uintptr_t base;
  uintptr_t caller_bridge;
  size_t span;
  int (*exercise)(const char *);
  int (*retire)(void *);
  int (*module_probe)(void);
  int (*fork_process)(void);
  void *(*native_thread)(void *);
  int (*native_fork)(void);
};
