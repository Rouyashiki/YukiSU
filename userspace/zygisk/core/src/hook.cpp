#include "core_bindings.hpp"

#include "plt_lifecycle.hpp"
#include "process_maps.hpp"
#include <yukiplt.hpp>

#include <jni.h>

#include <algorithm>
#include <android/fdsan.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <malloc.h>
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
constexpr char kZygoteInitClass[] = "com/android/internal/os/ZygoteInit";
constexpr char kAndroidRuntime[] = "/libandroid_runtime.so";
constexpr char kArtPostForkChild[] = "_ZN3art3jit3Jit19PostForkChildActionEbb";

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

#if defined(__aarch64__) || defined(__arm__)
extern "C" {
void yz_jni_entry_0();
void yz_jni_entry_1();
void yz_jni_entry_2();
void yz_jni_entry_3();
void yz_jni_entry_4();
void yz_jni_entry_5();
void yz_jni_entry_6();
void yz_jni_entry_7();
void yz_jni_entry_8();
void yz_jni_entry_9();

/* Target slots read by the entry stubs in jni_entry.S. */
__attribute__((visibility("hidden"))) void *g_yz_jni_targets[10] = {};
}
#endif

std::atomic<unsigned> g_plt_callbacks{0};
struct PltCallback {
  PltCallback() { g_plt_callbacks.fetch_add(1, std::memory_order_acq_rel); }
  ~PltCallback() { g_plt_callbacks.fetch_sub(1, std::memory_order_acq_rel); }
  PltCallback(const PltCallback &) = delete;
  PltCallback &operator=(const PltCallback &) = delete;
  PltCallback(PltCallback &&) = delete;
  PltCallback &operator=(PltCallback &&) = delete;
};
bool g_strdup_quiet = false;
bool g_fork_quiet = false;
bool g_art_quiet = false;
std::atomic<pid_t> g_art_prepared_pid{0};
std::atomic<pid_t> g_art_completed_pid{0};
bool g_art_requires_callback = false;
void retire_child_plt(bool include_art);

yukiplt::OriginalSlot<char *(*)(const char *)> g_strdup;
yukiplt::HookHandle g_strdup_handle;
yukiplt::OperationDiagnostic g_strdup_diagnostic;
bool g_strdup_recovery = false;
yukiplt::OriginalSlot<void (*)(void *, bool, bool)> g_art_post_fork_child;
yukiplt::HookHandle g_art_handle;
yukiplt::OperationDiagnostic g_art_diagnostic;
bool g_art_recovery = false;
bool g_jni_hooked = false;
enum class ArtHookState {
  NotAttempted,
  Deferred,
  Installed,
  Failed,
  NeedsCleanup,
  Retired
};
ArtHookState g_art_state = ArtHookState::NotAttempted;
pid_t g_zygote_pid = 0;

void hook_zygote_jni();
void new_art_post_fork_child(void *jit, bool is_system_server, bool is_zygote);

struct RuntimeHeaderRange {
  uintptr_t start = 0;
  size_t size = 0;
};

yukiplt::NamedImage g_runtime_image{};
yukiplt::NamedImage g_art_image{};
bool g_runtime_ready = false;

yukiplt::Result<yukiplt::NamedImage> find_unique_image(const char *name) {
  const auto images =
      yukiplt::find_images({yukiplt::ImageMatch::kBasename, name, 0});
  if (!images)
    return images.error();
  if (images.value().size() != 1)
    return yukiplt::Error::Make(yukiplt::ErrorCode::kAmbiguousImage);
  return images.value().front();
}

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
  PltCallback callback;
  if (!g_jni_hooked && str != nullptr && std::strcmp(str, kZygoteInit) == 0) {
    g_jni_hooked = true;
    ZLOGI("reached ZygoteInit -- VM is up, JNI is now safe");
    hook_zygote_jni();
  }
  return g_strdup.load() ? g_strdup.load()(str) : nullptr;
}

bool reset_art_method_fd(void *jit, size_t jit_size, int fd) {
  const uint64_t tag = android_fdsan_get_owner_tag(fd);
  if (tag >> 56 != ANDROID_FDSAN_OWNER_TYPE_UNIQUE_FD)
    return false;
  constexpr uint64_t kAddressMask = (uint64_t{1} << 56) - 1;
  const auto base = reinterpret_cast<uintptr_t>(jit);
  const uint64_t address = tag & kAddressMask;
  const uint64_t untagged_base = base & kAddressMask;
  if (jit_size < sizeof(int) || address < untagged_base ||
      address - untagged_base > jit_size - sizeof(int) ||
      address % alignof(int) != 0)
    return false;

  // fdsan identifies the owning unique_fd, avoiding a versioned Jit offset.
  auto *slot = static_cast<unsigned char *>(jit) + (address - untagged_base);
  int owned_fd;
  memcpy(&owned_fd, slot, sizeof(owned_fd));
  if (owned_fd != fd)
    return false;
  const int invalid_fd = -1;
  memcpy(slot, &invalid_fd, sizeof(invalid_fd));
  android_fdsan_close_with_tag(fd, tag);
  return true;
}

void hide_art_method_fds(void *jit) {
  if (jit == nullptr || android_fdsan_close_with_tag == nullptr)
    return;
  DIR *dir = opendir("/proc/self/fd");
  if (dir == nullptr)
    return;
  const size_t jit_size = malloc_usable_size(jit);
  const int dir_fd = dirfd(dir);
  while (dirent *entry = readdir(dir)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
      continue;
    char *end = nullptr;
    const long value = strtol(entry->d_name, &end, 10);
    if (end == nullptr || *end != '\0' || value < 0 || value > INT_MAX ||
        value == dir_fd)
      continue;
    const int fd = static_cast<int>(value);
    char target[128];
    const ssize_t length =
        readlinkat(dir_fd, entry->d_name, target, sizeof(target));
    if (length <= 0 || static_cast<size_t>(length) == sizeof(target))
      continue;
    const std::string_view path(target, static_cast<size_t>(length));
    if (path != "/memfd:/boot-image-methods.art" &&
        path != "/memfd:/boot-image-methods.art (deleted)")
      continue;
    if (reset_art_method_fd(jit, jit_size, fd))
      ZLOGI("closed inherited ART methods fd before child JIT setup");
    else
      ZLOGE("ART methods fd owner is unverified; leaving it intact");
  }
  closedir(dir);
}

void new_art_post_fork_child(void *jit, bool is_system_server, bool is_zygote) {
  PltCallback callback;
  if (g_zygote_pid != 0 && getpid() != g_zygote_pid) {
    const int saved_errno = errno;
    // Clear ART's owner before it can queue MapBootImageMethodsTask. The
    // child keeps its existing image pages; the parent's shared fd is intact.
    hide_art_method_fds(jit);
    g_art_prepared_pid.store(getpid(), std::memory_order_release);
    if (!is_zygote)
      retire_child_plt(true);
    errno = saved_errno;
  }
  g_art_post_fork_child.load()(jit, is_system_server, is_zygote);
  if (getpid() != g_zygote_pid)
    g_art_completed_pid.store(getpid(), std::memory_order_release);
}

int current_thread_count() {
  DIR *dir = opendir("/proc/self/task");
  if (dir == nullptr)
    return -1;
  int count = 0;
  errno = 0;
  while (dirent *entry = readdir(dir)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
      ++count;
  }
  const int error = errno;
  const int closed = closedir(dir);
  return error == 0 && closed == 0 ? count : -1;
}

void hook_art_post_fork_child(bool early) {
  yukizygisk::plt_lifecycle::Operation operation;
  if (!operation || (g_art_state != ArtHookState::NotAttempted &&
                     (early || g_art_state != ArtHookState::Deferred)))
    return;
  try {
    auto art = find_unique_image("libart.so");
    if (!art && art.error().code == yukiplt::ErrorCode::kImageNotFound)
      art = find_unique_image("libartd.so");
    if (!art) {
      g_art_state = early ? ArtHookState::Deferred : ArtHookState::Failed;
      ZLOGE("ART discovery: %s", yukiplt::error_name(art.error().code));
      return;
    }
    g_art_image = std::move(art.value());
    const yukiplt::HookRequest request{
        kArtPostForkChild, reinterpret_cast<void *>(new_art_post_fork_child),
        g_art_post_fork_child.output_address()};
    yukiplt::SlotReport slot;
    auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
        g_art_image, &request, 1, &slot, &g_art_diagnostic);
    const bool installed = result.ok();
    g_art_requires_callback =
        installed || g_art_diagnostic.stage >= yukiplt::OperationStage::kCommit;
    g_art_handle = std::move(result.handle);
    yukizygisk::plt_lifecycle::set_recovery(g_art_recovery,
                                            !installed && g_art_handle.valid());
    g_art_state =
        installed        ? ArtHookState::Installed
        : g_art_recovery ? ArtHookState::NeedsCleanup
        : early && g_art_diagnostic.stage < yukiplt::OperationStage::kCommit
            ? ArtHookState::Deferred
            : ArtHookState::Failed;
    ZLOGI("ART hook: early=%d threads=%d result=%s cause=%s errno=%d slot=%p "
          "state=%d",
          early, current_thread_count(), yukiplt::error_name(result.error.code),
          yukiplt::error_cause_name(g_art_diagnostic.cause),
          result.error.sys_errno, reinterpret_cast<void *>(slot.address),
          static_cast<int>(g_art_state));
  } catch (const std::bad_alloc &) {
    g_art_state = early ? ArtHookState::Deferred : ArtHookState::Failed;
    ZLOGE("ART preparation: OutOfMemory");
  } catch (...) {
    g_art_state = ArtHookState::Failed;
    ZLOGE("ART preparation failed");
  }
}

struct ZygiskContext {
  JNIEnv *env = nullptr;
  pid_t pid = -1;
  bool fork_attempted = false;
  int fork_errno = 0;
  jboolean *child_zygote = nullptr;
  jintArray *fds_to_ignore = nullptr; // app fork only -- the exempt channel
  std::vector<bool> allowed_fds;
  std::vector<int> exempted_fds;
};

ZygiskContext *g_ctx = nullptr;
yukiplt::OriginalSlot<int (*)()> g_orig_fork;
yukiplt::HookHandle g_fork_handle;
yukiplt::OperationDiagnostic g_fork_diagnostic;
bool g_fork_recovery = false;

void retire_child_plt(bool include_art) {
  if (g_zygote_pid == 0 || getpid() == g_zygote_pid)
    return;
  const int threads = current_thread_count();
  yukizygisk::plt_lifecycle::Operation operation(
      yukizygisk::plt_lifecycle::Access::Recover);
  if (!operation)
    return;
  struct Owned {
    yukiplt::HookHandle *handle;
    yukiplt::OperationDiagnostic *diagnostic;
    bool *recovery;
    bool *quiet;
  };
  const Owned hooks[] = {
      {&g_fork_handle, &g_fork_diagnostic, &g_fork_recovery, &g_fork_quiet},
      {&g_strdup_handle, &g_strdup_diagnostic, &g_strdup_recovery,
       &g_strdup_quiet},
      {&g_art_handle, &g_art_diagnostic, &g_art_recovery, &g_art_quiet}};
  for (size_t i = 0; i < (include_art ? 3U : 2U); ++i) {
    const auto &hook = hooks[i];
    if (i == 2 && hook.handle->valid() &&
        g_art_prepared_pid.load(std::memory_order_acquire) != getpid())
      continue;
    if (hook.handle->valid()) {
      yukiplt::SlotReport slot;
      const auto error =
          yukiplt::unhook_diagnosed(*hook.handle, &slot, 1, hook.diagnostic);
      yukizygisk::plt_lifecycle::set_recovery(*hook.recovery,
                                              hook.handle->valid());
      if (!error.ok())
        ZLOGE("child PLT retirement: %s errno=%d consumed=%d",
              yukiplt::error_name(error.code), error.sys_errno,
              hook.diagnostic->handle_consumed);
    }
    if (!hook.handle->valid() && threads == 1)
      *hook.quiet = true;
  }
  if (include_art && !g_art_handle.valid())
    g_art_state = ArtHookState::Retired;
  ZLOGI("child PLT retirement: threads=%d fork=%d bootstrap=%d art=%d", threads,
        g_fork_quiet, g_strdup_quiet, g_art_quiet);
}

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

/* App zygotes are named after the owning package (com.example_zygote) and run
 * as that package's uid; they only fork isolated processes, which never load
 * modules, so the core is disposable once preload is done. Bare names such as
 * webview_zygote or vendor stub_zygote fork ordinary processes that still need
 * module injection, so they keep it. */
bool yz_app_zygote_can_unmap(JNIEnv *env, jstring nice_name, jint uid) {
  if (nice_name == nullptr)
    return false;
  const char *name = env->GetStringUTFChars(nice_name, nullptr);
  if (name == nullptr) {
    env->ExceptionClear();
    return false;
  }
  const size_t len = strlen(name);
  constexpr char kSuffix[] = "_zygote";
  constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
  const bool named_app_zygote = len > kSuffixLen + 1 &&
                                strcmp(name + len - kSuffixLen, kSuffix) == 0 &&
                                memchr(name, '.', len - kSuffixLen) != nullptr;
  const int app_id = static_cast<int>(uid % 100000);
  const bool app_uid = app_id >= 10000 && app_id <= 19999;
  if (named_app_zygote)
    ZLOGI("app zygote %s (uid=%d, app_id=%d): %s", name, static_cast<int>(uid),
          app_id, app_uid ? "retiring child hooks" : "keeping zygote hooks");
  env->ReleaseStringUTFChars(nice_name, name);
  return named_app_zygote && app_uid;
}

void finish_app_child(JNIEnv *env, bool can_unmap, bool isolated) {
  // Retire child-only state; zygotes that inject descendants keep their hooks.
  if (!can_unmap)
    return;
  zygisk_self_destruct(env, isolated);
}

/* The native specialize fork becomes a no-op after ctx_fork_pre. */
int new_fork() {
  PltCallback callback;
  if (g_ctx != nullptr && (g_ctx->fork_attempted || g_ctx->pid >= 0)) {
    if (g_ctx->pid < 0)
      errno = g_ctx->fork_errno;
    if (g_ctx->pid == 0 &&
        (g_ctx->child_zygote == nullptr || *g_ctx->child_zygote == JNI_FALSE))
      retire_child_plt(false);
    return g_ctx->pid;
  }
  return yukizygisk::plt_lifecycle::fork_process(g_orig_fork.load());
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
  ctx->fork_attempted = true;
  ctx->pid = yukizygisk::plt_lifecycle::fork_process(g_orig_fork.load());
  ctx->fork_errno = ctx->pid < 0 ? errno : 0;
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
  kInitNativeState,
  kNativeZygoteInit,
  kZygoteMethodCount,
};

bool g_app_specialize_hooks_complete = false;
bool g_zygote_uses_fallback = false;
bool g_zygote_primary = false;

#if defined(__aarch64__) || defined(__arm__)
static_assert(kZygoteMethodCount == 10, "jni_entry.S provides ten stubs");
/* Installed through RegisterNatives so the caller context is captured. */
void *const kYzJniEntryTable[kZygoteMethodCount] = {
    reinterpret_cast<void *>(yz_jni_entry_0),
    reinterpret_cast<void *>(yz_jni_entry_1),
    reinterpret_cast<void *>(yz_jni_entry_2),
    reinterpret_cast<void *>(yz_jni_entry_3),
    reinterpret_cast<void *>(yz_jni_entry_4),
    reinterpret_cast<void *>(yz_jni_entry_5),
    reinterpret_cast<void *>(yz_jni_entry_6),
    reinterpret_cast<void *>(yz_jni_entry_7),
    reinterpret_cast<void *>(yz_jni_entry_8),
    reinterpret_cast<void *>(yz_jni_entry_9),
};
#endif

// Lambda bodies are not executed while initializing this function table.
// NOLINTNEXTLINE(cert-err58-cpp)
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
           ctx.child_zygote = &is_child_zygote;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           bool run_modules =
               ctx.pid == 0 && !is_isolated(uid) && !is_child_zygote;
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
             finish_app_child(env,
                              !is_child_zygote ||
                                  yz_app_zygote_can_unmap(env, nice_name, uid),
                              is_isolated(uid));
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
           ctx.child_zygote = &is_child_zygote;
           g_ctx = &ctx;
           const bool fifo_ui =
               is_top_app == JNI_TRUE && use_fifo_ui == JNI_TRUE;
           ctx_fork_pre(&ctx, fifo_ui);
           bool run_modules =
               ctx.pid == 0 && !is_isolated(uid) && !is_child_zygote;
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
             finish_app_child(env,
                              !is_child_zygote ||
                                  yz_app_zygote_can_unmap(env, nice_name, uid),
                              is_isolated(uid));
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
           ctx.child_zygote = &is_child_zygote;
           g_ctx = &ctx;
           ctx_fork_pre(&ctx); // real fork; child snapshots fds
           bool run_modules =
               ctx.pid == 0 && !is_isolated(uid) && !is_child_zygote;
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
             finish_app_child(env,
                              !is_child_zygote ||
                                  yz_app_zygote_can_unmap(env, nice_name, uid),
                              is_isolated(uid));
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
           ctx.child_zygote = &is_child_zygote;
           g_ctx = &ctx;
           const bool fifo_ui =
               is_top_app == JNI_TRUE && use_fifo_ui == JNI_TRUE;
           ctx_fork_pre(&ctx, fifo_ui);
           bool run_modules =
               ctx.pid == 0 && !is_isolated(uid) && !is_child_zygote;
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
             finish_app_child(env,
                              !is_child_zygote ||
                                  yz_app_zygote_can_unmap(env, nice_name, uid),
                              is_isolated(uid));
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
           bool run_modules = !is_isolated(uid) && !is_child_zygote;
           const bool can_unmap =
               !is_child_zygote || yz_app_zygote_can_unmap(env, nice_name, uid);
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
           finish_app_child(env, can_unmap, is_isolated(uid));
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
           bool run_modules = !is_isolated(uid) && !is_child_zygote;
           const bool can_unmap =
               !is_child_zygote || yz_app_zygote_can_unmap(env, nice_name, uid);
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
           finish_app_child(env, can_unmap, is_isolated(uid));
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
           bool run_modules = !is_isolated(uid) && !is_child_zygote;
           const bool can_unmap =
               !is_child_zygote || yz_app_zygote_can_unmap(env, nice_name, uid);
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
           finish_app_child(env, can_unmap, is_isolated(uid));
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
    /* Native state init runs once per zygote after the VM is up. */
    {"nativeInitNativeState", "(Z)V",
     reinterpret_cast<void *>(+[](JNIEnv *env, jclass clazz,
                                  jboolean is_primary) -> void {
       reinterpret_cast<void (*)(JNIEnv *, jclass, jboolean)>(
           g_zygote_methods[kInitNativeState].fnPtr)(env, clazz, is_primary);
       g_zygote_primary = is_primary == JNI_TRUE;
       ZLOGI("zygote native state: %s",
             g_zygote_primary ? "primary" : "secondary");
     })},
    // ZygoteInit calls this before applicationInit; childZygoteInit skips it.
    {"nativeZygoteInit", "()V",
     reinterpret_cast<void *>(+[](JNIEnv *env, jclass clazz) -> void {
       reinterpret_cast<void (*)(JNIEnv *, jclass)>(
           g_zygote_methods[kNativeZygoteInit].fnPtr)(env, clazz);
       if (g_zygote_pid != 0 && getpid() != g_zygote_pid)
         finish_app_child(env, true, is_isolated(getuid()));
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

bool reflected_is_native(JNIEnv *env, jobject method) {
  if (method == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  jclass type = env->GetObjectClass(method);
  if (type == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    if (type != nullptr)
      env->DeleteLocalRef(type);
    return false;
  }
  auto *const get_modifiers = env->GetMethodID(type, "getModifiers", "()I");
  env->DeleteLocalRef(type);
  if (get_modifiers == nullptr || env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  const jint modifiers = env->CallIntMethod(method, get_modifiers);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  constexpr jint native_modifier = 0x0100;
  return (modifiers & native_modifier) != 0;
}

void hook_jni_methods(JNIEnv *env, const char *clz, JNINativeMethod *methods,
                      int count, yuki::ihook::UnhookMode mode, int owner,
                      size_t first_entry = 0) {
  jclass clazz = env->FindClass(clz);
  if (clazz == nullptr) {
    env->ExceptionClear();
    ZLOGE("FindClass(%s) failed", clz);
    return;
  }

  if (count > 0)
    g_jni_hooks.reserve(g_jni_hooks.size() + static_cast<size_t>(count));

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
    if (!reflected_is_native(env, reflected)) {
      if (reflected != nullptr)
        env->DeleteLocalRef(reflected);
      m.fnPtr = nullptr;
      continue;
    }
    void *art = yuki::art::art_method_of(env, reflected);
    if (reflected != nullptr)
      env->DeleteLocalRef(reflected);
    void *orig = art ? yuki::art::native_entry(art) : nullptr;
    if (orig == nullptr) {
      env->ExceptionClear();
      ZLOGE("no original entry for %s", m.name);
      m.fnPtr = nullptr;
      continue;
    }

    // Zygote lifecycle hooks register through ART's native table instead of
    // patching libandroid_runtime text: an inline patch COWs the text page in
    // zygote, and every forked child then inherits it as shared-dirty
    // executable memory. Tango additionally translates the guest ARM32 native
    // entry and may not execute an inline-patched ArtMethod body, so the
    // specialization wrapper must stay reachable through the native table.
    const bool prefer_register = owner == -1;
    // Registering bypasses the inline capture trampoline, so route the zygote
    // table through an entry stub that records the caller context first.
    void *entry = m.fnPtr;
#if defined(__aarch64__) || defined(__arm__)
    if (prefer_register && first_entry < kZygoteMethodCount &&
        methods == g_zygote_methods.data() + first_entry &&
        static_cast<size_t>(i) < kZygoteMethodCount - first_entry) {
      const size_t index = first_entry + static_cast<size_t>(i);
      g_yz_jni_targets[index] = m.fnPtr;
      entry = kYzJniEntryTable[index];
    }
#endif

    JniHookRecord record;
    record.owner = owner;
    record.method = art;
    record.mode = mode;
    record.original = orig;
    record.replacement = entry;
    void *tramp = prefer_register
                      ? nullptr
                      : yuki::ihook::install(orig, m.fnPtr, &record.hook, false,
                                             owner == -1);
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
    m.fnPtr = entry;
    const jint registered = env->RegisterNatives(clazz, &m, 1);
    if (registered != JNI_OK || env->ExceptionCheck()) {
      env->ExceptionClear();
      ZLOGE("RegisterNatives failed for %s", m.name);
      m.fnPtr = nullptr;
    } else {
      if (owner == -1)
        g_zygote_uses_fallback = true;
      m.fnPtr = orig;
      ZLOGI("registered %s through ART", m.name);
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
      if (ok) {
        env->DeleteGlobalRef(record.clazz);
      }
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
  for (auto &m : yukizygisk::MapInfo::Scan()) {
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
  bool fork_plt_hooked = false;
  {
    yukizygisk::plt_lifecycle::Operation operation;
    if (operation && g_runtime_ready && !g_fork_handle.valid() &&
        g_fork_diagnostic.stage < yukiplt::OperationStage::kCommit) {
      const yukiplt::HookRequest request{"fork",
                                         reinterpret_cast<void *>(new_fork),
                                         g_orig_fork.output_address()};
      yukiplt::SlotReport slot;
      auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
          g_runtime_image, &request, 1, &slot, &g_fork_diagnostic);
      fork_plt_hooked = result.ok();
      g_fork_handle = std::move(result.handle);
      yukizygisk::plt_lifecycle::set_recovery(
          g_fork_recovery, !result.ok() && g_fork_handle.valid());
      if (!result.ok())
        ZLOGE("fork hook: %s cause=%s errno=%d",
              yukiplt::error_name(result.error.code),
              yukiplt::error_cause_name(g_fork_diagnostic.cause),
              result.error.sys_errno);
    }
  }
  if (!fork_plt_hooked) {
    ZLOGE("fork hook failed -- not hooking zygote natives");
    yz_unmap_injection_stub();
    return;
  }
  ZLOGI("fork hook armed (orig=%p)",
        reinterpret_cast<void *>(g_orig_fork.load()));
  hook_jni_methods(env, kZygote, g_zygote_methods.data(),
                   static_cast<int>(kNativeZygoteInit),
                   yuki::ihook::UnhookMode::DiscardCowPages, -1);
  hook_jni_methods(
      env, kZygoteInitClass, g_zygote_methods.data() + kNativeZygoteInit, 1,
      yuki::ihook::UnhookMode::DiscardCowPages, -1, kNativeZygoteInit);
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
  g_zygote_pid = getpid();
  hook_art_post_fork_child(false);
  yz_unmap_injection_stub();
  if (g_app_specialize_hooks_complete && system_server_hooked) {
    ZLOGI("zygote JNI takeover done");
  } else {
    ZLOGE("zygote JNI takeover incomplete: fork=%d specialize=%d server=%d",
          fork_hooked, specialize_hooked, system_server_hooked);
  }
}

} // namespace

/* Whether an entry stub has recorded a usable caller continuation. */
bool yz_capture_ready() {
#if defined(__aarch64__)
  // g_yz_ret_ctx: x19-x28, x29, x30, sp, then d8-d15.
  return g_yz_ret_ctx[11] != 0 && g_yz_ret_ctx[12] != 0;
#elif defined(__arm__)
  // g_yz_ret_ctx: r4-r11, lr, sp, then d8-d15.
  return g_yz_ret_ctx[8] != 0 && g_yz_ret_ctx[9] != 0;
#else
  return false;
#endif
}

/* The captured return address must land outside the core image. */
bool yz_capture_outside(uintptr_t base, size_t span) {
#if defined(__aarch64__)
  const uintptr_t resume = static_cast<uintptr_t>(g_yz_ret_ctx[11]);
#elif defined(__arm__)
  const uintptr_t resume = static_cast<uintptr_t>(g_yz_ret_ctx[8]);
#else
  (void)base;
  (void)span;
  return false;
#endif
  return resume < base || resume - base >= span;
}

/* Drop libandroid_runtime header pages faulted by symbol scans. */
void yz_drop_runtime_header_pages() {
  RuntimeHeaderRange range;
  if (core_system_iterate_phdr(find_runtime_header, &range) != 0 &&
      range.size != 0)
    madvise(reinterpret_cast<void *>(range.start), range.size, MADV_DONTNEED);
}

/* Install the early strdup hook. */
bool zygisk_hook_bootstrap(const char *self_path) {
  ZLOGI("hook bootstrap, self=%s", self_path ? self_path : "(null)");
  g_zygote_pid = getpid();
  {
    yukizygisk::plt_lifecycle::Operation operation;
    if (!operation || g_strdup_handle.valid() ||
        g_strdup_diagnostic.stage >= yukiplt::OperationStage::kCommit)
      return false;
    try {
      auto image = find_unique_image("libandroid_runtime.so");
      if (!image) {
        ZLOGE("bootstrap image: %s", yukiplt::error_name(image.error().code));
        return false;
      }
      g_runtime_image = std::move(image.value());
      g_runtime_ready = true;
      const yukiplt::HookRequest request{"strdup",
                                         reinterpret_cast<void *>(new_strdup),
                                         g_strdup.output_address()};
      yukiplt::SlotReport slot;
      auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
          g_runtime_image, &request, 1, &slot, &g_strdup_diagnostic);
      g_strdup_handle = std::move(result.handle);
      yukizygisk::plt_lifecycle::set_recovery(
          g_strdup_recovery, !result.ok() && g_strdup_handle.valid());
      if (!result.ok()) {
        ZLOGE("bootstrap hook: %s cause=%s errno=%d",
              yukiplt::error_name(result.error.code),
              yukiplt::error_cause_name(g_strdup_diagnostic.cause),
              result.error.sys_errno);
        return false;
      }
    } catch (...) {
      ZLOGE("bootstrap preparation failed");
      return false;
    }
  }
  ZLOGI("lifecycle bootstrap armed on %s", g_runtime_image.path.c_str());
  hook_art_post_fork_child(true);
  return true;
}

void zygisk_hook_jni_methods(JNIEnv *env, const char *cls,
                             JNINativeMethod *methods, int n, int owner) {
  hook_jni_methods(env, cls, methods, n, yuki::ihook::UnhookMode::RestoreBytes,
                   owner);
}

bool zygisk_exempt_fd(int fd) {
  if (g_ctx == nullptr || g_ctx->fds_to_ignore == nullptr || fd < 0)
    return false;
  g_ctx->exempted_fds.push_back(fd);
  return true;
}

bool zygisk_specialize_fully_inline_hooked() {
  return g_app_specialize_hooks_complete && !g_zygote_uses_fallback;
}

bool zygisk_original_output_allowed(void **output) {
  return output == nullptr ||
         (output != g_strdup.output_address() &&
          output != g_orig_fork.output_address() &&
          output != g_art_post_fork_child.output_address());
}

bool zygisk_restore_module_hooks(JNIEnv *env, int owner) {
  return owner >= 0 && restore_jni_hooks(env, owner);
}

/* Restore this core's hooks without touching module-owned hooks. */
CoreCleanupResult zygisk_self_unhook(JNIEnv *env) {
  CoreCleanupResult result;
  retire_child_plt(true);
  result.jni_restored = restore_jni_hooks(env, -1);
  result.plt_pending =
      g_art_handle.valid() || g_fork_handle.valid() || g_strdup_handle.valid();
  result.plt_quiescent =
      g_fork_quiet && g_strdup_quiet && g_art_quiet &&
      g_plt_callbacks.load(std::memory_order_acquire) == 0 &&
      (!g_art_requires_callback ||
       g_art_completed_pid.load(std::memory_order_acquire) == getpid());
  // Post-specialize descriptors may belong to resident modules or Java.
  // A module-directory pathname does not transfer their ownership to Core.
  ZLOGI("core cleanup: jni=%d plt_pending=%d quiescent=%d", result.jni_restored,
        result.plt_pending, result.plt_quiescent);
  return result;
}

bool zygisk_release_hook_storage() {
  if (!g_jni_hooks.empty() || g_art_handle.valid() || g_fork_handle.valid() ||
      g_strdup_handle.valid() ||
      g_plt_callbacks.load(std::memory_order_acquire) != 0)
    return false;
  std::vector<JniHookRecord>().swap(g_jni_hooks);
  std::string().swap(g_runtime_image.path);
  std::string().swap(g_art_image.path);
  if (g_ctx != nullptr) {
    std::vector<bool>().swap(g_ctx->allowed_fds);
    std::vector<int>().swap(g_ctx->exempted_fds);
    g_ctx = nullptr;
  }
  return true;
}
