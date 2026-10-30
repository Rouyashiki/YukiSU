#pragma once

#include <jni.h>

#include <cstdint>

#include "zygisk.hpp"

/* Early lifecycle hook. */
bool zygisk_hook_bootstrap(const char *self_path);
void zygisk_cleanup_tango_stub();

/* 0=inject, 1=inject+umount, 2=skip+umount. */
int zygisk_inject_decision(int uid);
struct CoreCleanupResult {
  bool jni_restored = false;
  bool plt_pending = false;
  bool plt_quiescent = false;
};
CoreCleanupResult zygisk_self_unhook(JNIEnv *env);
bool zygisk_release_hook_storage();
bool zygisk_original_output_allowed(void **output);
bool zygisk_restore_module_hooks(JNIEnv *env, int owner);
bool yz_capture_ready();
bool yz_capture_outside(uintptr_t base, size_t span);
void yz_drop_runtime_header_pages();
bool zygisk_specialize_fully_inline_hooked();
void zygisk_self_destruct(JNIEnv *env, bool isolated);
bool zygisk_app_core_unload_safe();
void zygisk_load_modules(JNIEnv *env);
void zygisk_run_app_pre(zygisk::AppSpecializeArgs *args);
void zygisk_run_app_post(JNIEnv *env, const zygisk::AppSpecializeArgs *args);
void zygisk_run_server_pre(zygisk::ServerSpecializeArgs *args);
void zygisk_run_server_post(JNIEnv *env,
                            const zygisk::ServerSpecializeArgs *args);

/* Module API hook glue. */
void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n, int owner);
bool zygisk_exempt_fd(int fd);
