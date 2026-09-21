#include <lsplt.hpp>

#include <jni.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <sched.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

#include "art_method.hpp"
#include "hook.hpp"
#include "inline_hook.hpp"
#include "log.hpp"

namespace {

constexpr char kZygoteInit[] = "com.android.internal.os.ZygoteInit";
constexpr char kZygote[] = "com/android/internal/os/Zygote";
constexpr char kAndroidRuntime[] = "/libandroid_runtime.so";

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

template <class F> struct Hook {
  F *original = nullptr;
};

Hook<char *(const char *)> g_strdup;
bool g_jni_hooked = false;

void hook_zygote_jni();

struct RuntimeHeaderRange {
  uintptr_t start = 0;
  size_t size = 0;
};

struct RuntimeMapCache {
  dev_t dev = 0;
  ino_t inode = 0;
  bool ready = false;
};

RuntimeMapCache g_runtime_maps;

int find_runtime_header(dl_phdr_info *info, size_t, void *data) {
  if (info == nullptr || info->dlpi_name == nullptr ||
      !ends_with(info->dlpi_name, kAndroidRuntime))
    return 0;

  auto *range = static_cast<RuntimeHeaderRange *>(data);
  const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
  if (page_size == 0 || (page_size & (page_size - 1)) != 0)
    return 0;
  for (size_t i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD || ph.p_offset != 0 || ph.p_flags != PF_R ||
        ph.p_filesz == 0)
      continue;
    uintptr_t segment = static_cast<uintptr_t>(
        static_cast<ElfW(Addr)>(info->dlpi_addr + ph.p_vaddr));
    if (ph.p_filesz > UINTPTR_MAX - segment ||
        segment + ph.p_filesz > UINTPTR_MAX - (page_size - 1))
      continue;
    uintptr_t start = segment & ~(page_size - 1);
    uintptr_t end = (segment + ph.p_filesz + page_size - 1) & ~(page_size - 1);
    if (end > start) {
      *range = {start, end - start};
      return 1;
    }
  }
  return 0;
}

/* ZygoteInit means JNI is ready. */
char *new_strdup(const char *str) {
  if (!g_jni_hooked && str != nullptr && std::strcmp(str, kZygoteInit) == 0) {
    g_jni_hooked = true;
    ZLOGI("reached ZygoteInit -- VM is up, JNI is now safe");
    hook_zygote_jni();
  }
  return g_strdup.original ? g_strdup.original(str) : nullptr;
}

bool cache_libandroid_runtime() {
  if (g_runtime_maps.ready)
    return true;

  for (const auto &map : lsplt::MapInfo::Scan()) {
    if (map.offset != 0 || !(map.perms & PROT_READ) || map.dev == 0 ||
        map.inode == 0 || !ends_with(map.path, kAndroidRuntime))
      continue;
    g_runtime_maps = {map.dev, map.inode, true};
    return true;
  }
  return false;
}

bool find_libandroid_runtime(dev_t &dev, ino_t &inode) {
  if (!g_runtime_maps.ready)
    return false;
  dev = g_runtime_maps.dev;
  inode = g_runtime_maps.inode;
  return true;
}

struct ZygiskContext {
  JNIEnv *env = nullptr;
  pid_t pid = -1; // <0 not forked; ==0 child; >0 zygote (parent)
  jintArray *fds_to_ignore = nullptr; // app fork only -- the exempt channel
  std::vector<bool> allowed_fds;
  std::vector<int> exempted_fds;
};

ZygiskContext *g_ctx = nullptr;
int (*g_orig_fork)() = nullptr;

bool is_isolated(int uid) {
  int app_id = uid % 100000;
  return app_id >= 90000 && app_id <= 99999;
}

int forked_decision(bool is_child, int uid) {
  if (!is_child)
    return 0;
  if (is_isolated(uid))
    return 2;
  return zygisk_inject_decision(uid);
}

void finish_app_child(JNIEnv *env, bool is_child_zygote, bool isolated) {
  // Every fully specialized app child drops the inherited core. Child zygotes
  // retain it because they must inject their own future descendants.
  if (is_child_zygote)
    return;
  if (!zygisk_app_core_unload_safe())
    return;
  zygisk_self_destruct(env, isolated);
}

/* The native specialize fork becomes a no-op after ctx_fork_pre. */
int new_fork() {
  return (g_ctx != nullptr && g_ctx->pid >= 0)
             ? g_ctx->pid
             : (g_orig_fork != nullptr ? g_orig_fork() : fork());
}

size_t fd_table_size() {
  long n = sysconf(_SC_OPEN_MAX);
  return n > 0 ? static_cast<size_t>(n) : 1024;
}

void mark_allowed(ZygiskContext *ctx, int fd) {
  if (fd >= 0 && static_cast<size_t>(fd) < ctx->allowed_fds.size())
    ctx->allowed_fds[fd] = true;
}

bool set_fifo_ui_scheduler(int policy, int priority) {
  sched_param param{};
  param.sched_priority = priority;
  if (sched_setscheduler(0, policy, &param) != 0) {
    ZLOGE("sched_setscheduler(%#x) failed: %s", policy, strerror(errno));
    return false;
  }
  return true;
}

[[noreturn]] void abort_fifo_ui_scheduler(JNIEnv *env) {
  env->FatalError("failed to apply FIFO UI scheduler state");
  _exit(127);
}

/* Real fork; child snapshots native fds. */
void ctx_fork_pre(ZygiskContext *ctx, bool fifo_ui = false) {
  if (fifo_ui)
    set_fifo_ui_scheduler(SCHED_FIFO, 1);
  ctx->pid = g_orig_fork != nullptr ? g_orig_fork() : fork();
  if (fifo_ui) {
    if (ctx->pid == 0) {
      if (!set_fifo_ui_scheduler(SCHED_FIFO | SCHED_RESET_ON_FORK, 1))
        abort_fifo_ui_scheduler(ctx->env);
    } else {
      if (!set_fifo_ui_scheduler(SCHED_OTHER, 0))
        abort_fifo_ui_scheduler(ctx->env);
    }
  }
  if (ctx->pid != 0)
    return; // zygote
  ctx->allowed_fds.assign(fd_table_size(), false);
  if (DIR *d = opendir("/proc/self/fd")) {
    int dfd = dirfd(d);
    while (dirent *e = readdir(d))
      mark_allowed(ctx, atoi(e->d_name));
    if (dfd >= 0 && static_cast<size_t>(dfd) < ctx->allowed_fds.size())
      ctx->allowed_fds[dfd] = false;
    closedir(d);
  }
}

void reconcile_fifo_ui_scheduler(ZygiskContext *ctx, bool armed, bool enabled) {
  if (ctx->pid == 0 && armed && !enabled &&
      !set_fifo_ui_scheduler(SCHED_OTHER, 0))
    abort_fifo_ui_scheduler(ctx->env);
}

/* Drop inherited module fds. */
int close_inherited_module_fds() {
  DIR *d = opendir("/proc/self/fd");
  if (d == nullptr)
    return 0;
  int dfd = dirfd(d);
  int closed = 0;
  char target[256];
  for (dirent *e; (e = readdir(d)) != nullptr;) {
    int fd = atoi(e->d_name);
    if (fd < 0 || fd == dfd)
      continue;
    ssize_t n = readlinkat(dfd, e->d_name, target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = '\0';
    if (strstr(target, "/data/adb/modules") != nullptr) {
      close(fd);
      ++closed;
    }
  }
  closedir(d);
  return closed;
}

/* Apply exemptFd and close non-native fds in the child. */
void ctx_sanitize_fds(ZygiskContext *ctx) {
  if (ctx->pid != 0)
    return;
  JNIEnv *env = ctx->env;

  if (ctx->fds_to_ignore != nullptr) {
    jintArray old = *ctx->fds_to_ignore;
    int old_len = old != nullptr ? env->GetArrayLength(old) : 0;
    if (old != nullptr) {
      int *p = env->GetIntArrayElements(old, nullptr);
      for (int i = 0; i < old_len; ++i)
        mark_allowed(ctx, p[i]);
      env->ReleaseIntArrayElements(old, p, JNI_ABORT);
    }
    if (!ctx->exempted_fds.empty()) {
      jintArray arr = env->NewIntArray(
          static_cast<jsize>(old_len + ctx->exempted_fds.size()));
      if (arr != nullptr) {
        if (old != nullptr) {
          int *p = env->GetIntArrayElements(old, nullptr);
          env->SetIntArrayRegion(arr, 0, old_len, p);
          env->ReleaseIntArrayElements(old, p, JNI_ABORT);
        }
        env->SetIntArrayRegion(arr, old_len,
                               static_cast<jsize>(ctx->exempted_fds.size()),
                               ctx->exempted_fds.data());
        for (int fd : ctx->exempted_fds)
          mark_allowed(ctx, fd);
        *ctx->fds_to_ignore = arr;
      }
    }
  }

  if (DIR *d = opendir("/proc/self/fd")) {
    int dfd = dirfd(d);
    while (dirent *e = readdir(d)) {
      int fd = atoi(e->d_name);
      if (fd < 0 || fd == dfd)
        continue;
      if (static_cast<size_t>(fd) >= ctx->allowed_fds.size() ||
          !ctx->allowed_fds[fd])
        close(fd);
    }
    closedir(d);
  }
}

/* Replacement zygote natives. */
enum ZygoteMethodIndex : size_t {
  kForkLegacy,
  kForkFifo,
  kForkSysprop,
  kForkCgroup,
  kSpecializeLegacy,
  kSpecializeSysprop,
  kSpecializeCgroup,
  kForkSystemServer,
  kZygoteMethodCount,
};

bool g_app_specialize_hooks_complete = false;
bool g_zygote_uses_fallback = false;

std::array<JNINativeMethod, kZygoteMethodCount> g_zygote_methods = {{
    {"nativeForkAndSpecialize",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jintArray fds_to_close,
             jintArray fds_to_ignore, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds();
           }
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jobjectArray, jobjectArray, jboolean,
               jboolean)>(g_zygote_methods[kForkLegacy].fnPtr);
           jint pid =
               orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                    mount_external, se_info, nice_name, fds_to_close,
                    fds_to_ignore, is_child_zygote, instruction_set,
                    app_data_dir, is_top_app, pkg_data_info_list,
                    allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           if (ctx.pid == 0)
             finish_app_child(env, is_child_zygote, is_isolated(uid));
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
           g_ctx = nullptr;
           return pid;
         })},
    {"nativeForkAndSpecialize",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;ZZ[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jintArray fds_to_close,
             jintArray fds_to_ignore, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jboolean use_fifo_ui, jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           const bool fifo_ui =
               is_top_app == JNI_TRUE && use_fifo_ui == JNI_TRUE;
           ctx_fork_pre(&ctx, fifo_ui);
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2)
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds();
           }
           reconcile_fifo_ui_scheduler(&ctx, fifo_ui,
                                       is_top_app == JNI_TRUE &&
                                           use_fifo_ui == JNI_TRUE);
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jboolean, jobjectArray, jobjectArray,
               jboolean, jboolean, jboolean)>(
               g_zygote_methods[kForkFifo].fnPtr);
           jint pid = orig(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, fds_to_close, fds_to_ignore,
               is_child_zygote, instruction_set, app_data_dir, is_top_app,
               use_fifo_ui, pkg_data_info_list, allowlisted_data_info,
               mount_data_dirs, mount_storage_dirs, mount_sysprop_overrides);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           if (ctx.pid == 0)
             finish_app_child(env, is_child_zygote, is_isolated(uid));
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
           g_ctx = nullptr;
           return pid;
         })},
    {"nativeForkAndSpecialize",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jintArray fds_to_close,
             jintArray fds_to_ignore, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env); // dlopen + onLoad here, in the child
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds(); // isolated: drop inherited leaks
           }
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jintArray, jintArray, jboolean, jstring,
               jstring, jboolean, jobjectArray, jobjectArray, jboolean,
               jboolean, jboolean)>(g_zygote_methods[kForkSysprop].fnPtr);
           jint pid = orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                           mount_external, se_info, nice_name, fds_to_close,
                           fds_to_ignore, is_child_zygote, instruction_set,
                           app_data_dir, is_top_app, pkg_data_info_list,
                           allowlisted_data_info, mount_data_dirs,
                           mount_storage_dirs, mount_sysprop_overrides);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           if (ctx.pid == 0)
             finish_app_child(env, is_child_zygote, is_isolated(uid));
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
           g_ctx = nullptr;
           return pid;
         })},
    {"nativeForkAndSpecialize",
     "(III[II[[IILjava/lang/String;Ljava/lang/String;[I[IZLjava/lang/String;"
     "Ljava/lang/String;ZZ[Ljava/lang/String;[Ljava/lang/String;ZZZ)I",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint cgroup_uid, jint gid,
             jintArray gids, jint runtime_flags, jobjectArray rlimits,
             jint mount_external, jstring se_info, jstring nice_name,
             jintArray fds_to_close, jintArray fds_to_ignore,
             jboolean is_child_zygote, jstring instruction_set,
             jstring app_data_dir, jboolean is_top_app, jboolean use_fifo_ui,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> jint {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.fds_to_ignore = &fds_to_ignore;
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           ZygiskContext ctx;
           ctx.env = env;
           ctx.fds_to_ignore = &fds_to_ignore;
           g_ctx = &ctx;
           const bool fifo_ui =
               is_top_app == JNI_TRUE && use_fifo_ui == JNI_TRUE;
           ctx_fork_pre(&ctx, fifo_ui);
           bool run_modules = ctx.pid == 0 && !is_isolated(uid);
           int decision = forked_decision(ctx.pid == 0, uid);
           if (decision == 2)
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
             ctx_sanitize_fds(&ctx);
           } else if (ctx.pid == 0) {
             close_inherited_module_fds();
           }
           reconcile_fifo_ui_scheduler(&ctx, fifo_ui,
                                       is_top_app == JNI_TRUE &&
                                           use_fifo_ui == JNI_TRUE);
           auto orig = reinterpret_cast<jint (*)(
               JNIEnv *, jclass, jint, jint, jint, jintArray, jint,
               jobjectArray, jint, jstring, jstring, jintArray, jintArray,
               jboolean, jstring, jstring, jboolean, jboolean, jobjectArray,
               jobjectArray, jboolean, jboolean, jboolean)>(
               g_zygote_methods[kForkCgroup].fnPtr);
           jint pid = orig(
               env, clazz, uid, cgroup_uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, fds_to_close, fds_to_ignore,
               is_child_zygote, instruction_set, app_data_dir, is_top_app,
               use_fifo_ui, pkg_data_info_list, allowlisted_data_info,
               mount_data_dirs, mount_storage_dirs, mount_sysprop_overrides);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           if (ctx.pid == 0)
             finish_app_child(env, is_child_zygote, is_isolated(uid));
           if (is_child_zygote && ctx.pid == 0)
             close_inherited_module_fds();
           g_ctx = nullptr;
           return pid;
         })},
    /* USAP specialize path. */
    {"nativeSpecializeAppProcess",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZ)V",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs) -> void {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           bool run_modules = !is_isolated(uid);
           int decision = forked_decision(true, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) { // USAP: skip isolated processes
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
           }
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jboolean, jstring, jstring, jboolean,
               jobjectArray, jobjectArray, jboolean, jboolean)>(
               g_zygote_methods[kSpecializeLegacy].fnPtr)(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           finish_app_child(env, is_child_zygote, is_isolated(uid));
         })},
    {"nativeSpecializeAppProcess",
     "(II[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)V",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint gid, jintArray gids,
             jint runtime_flags, jobjectArray rlimits, jint mount_external,
             jstring se_info, jstring nice_name, jboolean is_child_zygote,
             jstring instruction_set, jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> void {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           bool run_modules = !is_isolated(uid);
           int decision = forked_decision(true, uid);
           if (decision == 2) // denylist + force mode: do not inject
             run_modules = false;
           if (run_modules) { // USAP: skip isolated processes
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
           }
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jintArray, jint, jobjectArray,
               jint, jstring, jstring, jboolean, jstring, jstring, jboolean,
               jobjectArray, jobjectArray, jboolean, jboolean, jboolean)>(
               g_zygote_methods[kSpecializeSysprop].fnPtr)(
               env, clazz, uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs,
               mount_sysprop_overrides);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           finish_app_child(env, is_child_zygote, is_isolated(uid));
         })},
    {"nativeSpecializeAppProcess",
     "(III[II[[IILjava/lang/String;Ljava/lang/String;ZLjava/lang/String;"
     "Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;ZZZ)V",
     reinterpret_cast<void *>(
         +[](JNIEnv *env, jclass clazz, jint uid, jint cgroup_uid, jint gid,
             jintArray gids, jint runtime_flags, jobjectArray rlimits,
             jint mount_external, jstring se_info, jstring nice_name,
             jboolean is_child_zygote, jstring instruction_set,
             jstring app_data_dir, jboolean is_top_app,
             jobjectArray pkg_data_info_list,
             jobjectArray allowlisted_data_info, jboolean mount_data_dirs,
             jboolean mount_storage_dirs,
             jboolean mount_sysprop_overrides) -> void {
           zygisk::AppSpecializeArgs args(
               uid, gid, gids, runtime_flags, rlimits, mount_external, se_info,
               nice_name, instruction_set, app_data_dir);
           args.is_child_zygote = &is_child_zygote;
           args.is_top_app = &is_top_app;
           args.pkg_data_info_list = &pkg_data_info_list;
           args.whitelisted_data_info_list = &allowlisted_data_info;
           args.mount_data_dirs = &mount_data_dirs;
           args.mount_storage_dirs = &mount_storage_dirs;
           args.mount_sysprop_overrides = &mount_sysprop_overrides;
           bool run_modules = !is_isolated(uid);
           int decision = forked_decision(true, uid);
           if (decision == 2)
             run_modules = false;
           if (run_modules) {
             zygisk_load_modules(env);
             zygisk_run_app_pre(&args);
           }
           reinterpret_cast<void (*)(
               JNIEnv *, jclass, jint, jint, jint, jintArray, jint,
               jobjectArray, jint, jstring, jstring, jboolean, jstring, jstring,
               jboolean, jobjectArray, jobjectArray, jboolean, jboolean,
               jboolean)>(g_zygote_methods[kSpecializeCgroup].fnPtr)(
               env, clazz, uid, cgroup_uid, gid, gids, runtime_flags, rlimits,
               mount_external, se_info, nice_name, is_child_zygote,
               instruction_set, app_data_dir, is_top_app, pkg_data_info_list,
               allowlisted_data_info, mount_data_dirs, mount_storage_dirs,
               mount_sysprop_overrides);
           if (run_modules)
             zygisk_run_app_post(env, &args);
           finish_app_child(env, is_child_zygote, is_isolated(uid));
         })},
    /* system_server fork. */
    {"nativeForkSystemServer", "(II[II[[IJJ)I",
     reinterpret_cast<void *>(+[](JNIEnv *env, jclass clazz, jint uid, jint gid,
                                  jintArray gids, jint runtime_flags,
                                  jobjectArray rlimits,
                                  jlong permitted_capabilities,
                                  jlong effective_capabilities) -> jint {
       zygisk::ServerSpecializeArgs args(uid, gid, gids, runtime_flags,
                                         permitted_capabilities,
                                         effective_capabilities);
       ZygiskContext ctx;
       ctx.env = env;
       ctx.fds_to_ignore = nullptr; // server fork has no exempt channel
       g_ctx = &ctx;
       ctx_fork_pre(&ctx);
       if (ctx.pid == 0) {         // child (system_server)
         zygisk_load_modules(env); // dlopen + onLoad here, in the child
         zygisk_run_server_pre(&args);
         ctx_sanitize_fds(&ctx);
       }
       auto orig =
           reinterpret_cast<jint (*)(JNIEnv *, jclass, jint, jint, jintArray,
                                     jint, jobjectArray, jlong, jlong)>(
               g_zygote_methods[kForkSystemServer].fnPtr);
       jint pid = orig(env, clazz, uid, gid, gids, runtime_flags, rlimits,
                       permitted_capabilities, effective_capabilities);
       if (ctx.pid == 0)
         zygisk_run_server_post(env, &args);
       g_ctx = nullptr;
       return pid;
     })},
}};

struct JniHookRecord {
  int owner = -1;
  void *method = nullptr;
  yuki::ihook::Hook hook;
  yuki::ihook::UnhookMode mode = yuki::ihook::UnhookMode::RestoreBytes;
  jclass clazz = nullptr;
  std::string name;
  std::string signature;
  void *original = nullptr;
  void *replacement = nullptr;
};
std::vector<JniHookRecord> g_jni_hooks;

void hook_jni_methods(JNIEnv *env, const char *clz, JNINativeMethod *methods,
                      int count, yuki::ihook::UnhookMode mode, int owner) {
  jclass clazz = env->FindClass(clz);
  if (clazz == nullptr) {
    env->ExceptionClear();
    ZLOGE("FindClass(%s) failed", clz);
    return;
  }

  for (int i = 0; i < count; ++i) {
    JNINativeMethod &m = methods[i];
    if (m.fnPtr == nullptr)
      continue;
    bool is_static = true;
    jmethodID mid = env->GetStaticMethodID(clazz, m.name, m.signature);
    if (mid == nullptr) {
      env->ExceptionClear();
      mid = env->GetMethodID(clazz, m.name, m.signature);
      is_static = false;
    }
    if (mid == nullptr) {
      env->ExceptionClear();
      m.fnPtr = nullptr;
      continue;
    }
    jobject reflected = env->ToReflectedMethod(clazz, mid, is_static);
    void *art = reflected ? yuki::art::art_method_of(env, reflected) : nullptr;
    if (reflected != nullptr)
      env->DeleteLocalRef(reflected);
    void *orig = art ? yuki::art::native_entry(art) : nullptr;
    if (orig == nullptr) {
      env->ExceptionClear();
      ZLOGE("no original entry for %s", m.name);
      m.fnPtr = nullptr;
      continue;
    }

    JniHookRecord record;
    record.owner = owner;
    record.method = art;
    record.mode = mode;
    record.original = orig;
    record.replacement = m.fnPtr;
    void *tramp =
        yuki::ihook::install(orig, m.fnPtr, &record.hook, false, owner == -1);
    if (tramp != nullptr) {
      g_jni_hooks.push_back(std::move(record));
      m.fnPtr = tramp;
      ZLOGI("inline-hooked %s @orig=%p tramp=%p", m.name, orig, tramp);
      continue;
    }

    record.clazz = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));
    if (record.clazz == nullptr) {
      env->ExceptionClear();
      m.fnPtr = nullptr;
      continue;
    }
    record.name = m.name;
    record.signature = m.signature;
    g_jni_hooks.push_back(std::move(record));
    if (env->RegisterNatives(clazz, &m, 1) != JNI_OK || env->ExceptionCheck()) {
      env->ExceptionClear();
      ZLOGE("RegisterNatives fallback failed for %s", m.name);
      m.fnPtr = nullptr;
    } else {
      if (owner == -1)
        g_zygote_uses_fallback = true;
      m.fnPtr = orig;
      ZLOGI("RegisterNatives fallback installed for %s", m.name);
    }
  }
  env->DeleteLocalRef(clazz);
}

bool restore_jni_hooks(JNIEnv *env, int owner) {
  bool restored = true;
  for (size_t i = g_jni_hooks.size(); i > 0; --i) {
    auto &record = g_jni_hooks[i - 1];
    if (record.owner != owner)
      continue;
    const bool chained =
        std::any_of(g_jni_hooks.begin() + static_cast<ptrdiff_t>(i),
                    g_jni_hooks.end(), [&](const JniHookRecord &later) {
                      return later.method == record.method ||
                             later.original == record.original;
                    });
    if (chained) {
      ZLOGE("JNI restore blocked by a later hook: module=%d", owner);
      restored = false;
      continue;
    }
    bool ok;
    if (record.clazz == nullptr) {
      ok = yuki::ihook::uninstall(&record.hook, record.mode);
    } else {
      void *current = yuki::art::native_entry(record.method);
      ok = env != nullptr &&
           (current == record.replacement || current == record.original);
      if (ok && current != record.original) {
        JNINativeMethod method{const_cast<char *>(record.name.c_str()),
                               const_cast<char *>(record.signature.c_str()),
                               record.original};
        ok = env->RegisterNatives(record.clazz, &method, 1) == JNI_OK;
        if (env->ExceptionCheck()) {
          env->ExceptionClear();
          ok = false;
        }
        ok = ok && yuki::art::native_entry(record.method) == record.original;
      }
      if (ok)
        env->DeleteGlobalRef(record.clazz);
    }
    if (!ok) {
      ZLOGE("JNI hook restore failed: module=%d", owner);
      restored = false;
      continue;
    }
    g_jni_hooks.erase(g_jni_hooks.begin() + static_cast<ptrdiff_t>(i - 1));
  }
  return restored;
}

/* Drop the spent AT_ENTRY stub. */
void yz_unmap_injection_stub() {
  zygisk_cleanup_tango_stub();
  for (auto &m : lsplt::MapInfo::Scan()) {
    if ((m.perms & (PROT_READ | PROT_WRITE | PROT_EXEC)) !=
            (PROT_READ | PROT_WRITE | PROT_EXEC) ||
        !m.path.empty() || m.end - m.start != 0x1000)
      continue;
    if (*reinterpret_cast<volatile uint32_t *>(m.start + 0x800) != 0x52414e21u)
      continue;
    munmap(reinterpret_cast<void *>(m.start), m.end - m.start);
    ZLOGI("unmapped kernel injection stub @%p",
          reinterpret_cast<void *>(m.start));
    return;
  }
}

void hook_zygote_jni() {
  auto get_vms = reinterpret_cast<jint (*)(JavaVM **, jsize, jsize *)>(
      dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
  if (get_vms == nullptr) {
    ZLOGE("JNI_GetCreatedJavaVMs not found");
    return;
  }
  JavaVM *vm = nullptr;
  jsize n = 0;
  if (get_vms(&vm, 1, &n) != JNI_OK || vm == nullptr) {
    ZLOGE("no created JavaVM");
    return;
  }
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK ||
      env == nullptr) {
    ZLOGE("GetEnv failed");
    return;
  }
  if (!yuki::art::probe(env)) {
    ZLOGE("ArtMethod layout probe failed; not hooking natives");
    return;
  }
  // Fork early; specialize then runs in our child.
  dev_t rt_dev = 0;
  ino_t rt_inode = 0;
  bool fork_plt_hooked = false;
  if (find_libandroid_runtime(rt_dev, rt_inode)) {
    fork_plt_hooked =
        lsplt::RegisterHook(rt_dev, rt_inode, "fork",
                            reinterpret_cast<void *>(new_fork),
                            reinterpret_cast<void **>(&g_orig_fork)) &&
        lsplt::CommitHook() && g_orig_fork != nullptr;
  }
  if (!fork_plt_hooked) {
    ZLOGE("fork hook failed -- not hooking zygote natives");
    yz_unmap_injection_stub();
    return;
  }
  ZLOGI("fork hook armed (orig=%p)", reinterpret_cast<void *>(g_orig_fork));
  hook_jni_methods(env, kZygote, g_zygote_methods.data(),
                   static_cast<int>(g_zygote_methods.size()),
                   yuki::ihook::UnhookMode::DiscardCowPages, -1);
  const bool fork_hooked = g_zygote_methods[kForkLegacy].fnPtr != nullptr ||
                           g_zygote_methods[kForkSysprop].fnPtr != nullptr ||
                           g_zygote_methods[kForkFifo].fnPtr != nullptr ||
                           g_zygote_methods[kForkCgroup].fnPtr != nullptr;
  const bool specialize_hooked =
      g_zygote_methods[kSpecializeLegacy].fnPtr != nullptr ||
      g_zygote_methods[kSpecializeSysprop].fnPtr != nullptr ||
      g_zygote_methods[kSpecializeCgroup].fnPtr != nullptr;
  const bool system_server_hooked =
      g_zygote_methods[kForkSystemServer].fnPtr != nullptr;
  g_app_specialize_hooks_complete = fork_hooked && specialize_hooked;
  yz_unmap_injection_stub();
  if (g_app_specialize_hooks_complete && system_server_hooked) {
    ZLOGI("zygote JNI takeover done");
  } else {
    ZLOGE("zygote JNI takeover incomplete: fork=%d specialize=%d server=%d",
          fork_hooked, specialize_hooked, system_server_hooked);
  }
}

} // namespace

/* Drop libandroid_runtime header pages faulted by symbol scans. */
void yz_drop_runtime_header_pages() {
  RuntimeHeaderRange range;
  if (dl_iterate_phdr(find_runtime_header, &range) != 0 && range.size != 0)
    madvise(reinterpret_cast<void *>(range.start), range.size, MADV_DONTNEED);
}

/* Install the early strdup hook. */
bool zygisk_hook_bootstrap(const char *self_path) {
  ZLOGI("hook bootstrap, self=%s", self_path ? self_path : "(null)");

  if (!cache_libandroid_runtime()) {
    ZLOGE("libandroid_runtime.so not mapped yet; cannot bootstrap");
    return false;
  }
  dev_t dev = 0;
  ino_t inode = 0;
  if (!find_libandroid_runtime(dev, inode))
    return false;

  if (!lsplt::RegisterHook(dev, inode, "strdup",
                           reinterpret_cast<void *>(new_strdup),
                           reinterpret_cast<void **>(&g_strdup.original))) {
    ZLOGE("RegisterHook(strdup) failed");
    return false;
  }
  if (!lsplt::CommitHook() || g_strdup.original == nullptr) {
    ZLOGE("CommitHook failed");
    return false;
  }

  ZLOGI("lifecycle bootstrap armed on libandroid_runtime (dev=%u,%u inode=%lu)",
        major(dev), minor(dev), static_cast<unsigned long>(inode));
  return true;
}

void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n, int owner) {
  hook_jni_methods(env, cls, methods, n, yuki::ihook::UnhookMode::RestoreBytes,
                   owner);
}

bool zygisk_plt_hook_register(dev_t dev, ino_t inode, const char *symbol,
                              void *new_func, void **old_func) {
  return lsplt::RegisterHook(dev, inode, symbol, new_func, old_func);
}

bool zygisk_plt_hook_commit() { return lsplt::CommitHookCached(); }

bool zygisk_exempt_fd(int fd) {
  if (g_ctx == nullptr || g_ctx->fds_to_ignore == nullptr || fd < 0)
    return false;
  g_ctx->exempted_fds.push_back(fd);
  return true;
}

bool zygisk_specialize_fully_inline_hooked() {
  return g_app_specialize_hooks_complete && !g_zygote_uses_fallback;
}

bool zygisk_restore_module_hooks(JNIEnv *env, int owner) {
  return owner >= 0 && restore_jni_hooks(env, owner);
}

/* Restore hooks before self-unmap. */
bool zygisk_self_unhook(JNIEnv *env) {
  const bool lifecycle_restored = restore_jni_hooks(env, -1);
  bool unhooked = lifecycle_restored && g_jni_hooks.empty();
  dev_t dev = 0;
  ino_t inode = 0;
  if (find_libandroid_runtime(dev, inode)) {
    bool registered = true;
    bool have_hook = false;
    if (g_strdup.original != nullptr) {
      have_hook = true;
      if (!lsplt::RegisterHook(dev, inode, "strdup",
                               reinterpret_cast<void *>(g_strdup.original),
                               nullptr))
        registered = false;
    }
    if (g_orig_fork != nullptr) {
      have_hook = true;
      if (!lsplt::RegisterHook(dev, inode, "fork",
                               reinterpret_cast<void *>(g_orig_fork), nullptr))
        registered = false;
    }
    if (have_hook) {
      const bool committed = lsplt::CommitHookCached();
      if (!registered || !committed)
        unhooked = false;
    }
  } else if (g_strdup.original != nullptr || g_orig_fork != nullptr) {
    unhooked = false;
  }
  int nc = close_inherited_module_fds();
  if (nc != 0)
    ZLOGI("self-destruct: closed %d leaked module fd", nc);
  if (unhooked && !lsplt::ReleaseInactiveCache())
    unhooked = false;
  if (unhooked) {
    std::vector<JniHookRecord>().swap(g_jni_hooks);
    if (g_ctx != nullptr) {
      std::vector<bool>().swap(g_ctx->allowed_fds);
      std::vector<int>().swap(g_ctx->exempted_fds);
      g_ctx = nullptr;
    }
  }
  return unhooked;
}
