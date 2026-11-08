// Drive the production JNI wrappers without ART or a running root daemon.
#define zygisk_load_modules fd_test_load_modules
#define zygisk_run_app_pre fd_test_run_app_pre
#define zygisk_run_app_post fd_test_run_app_post
#define zygisk_inject_decision fd_test_inject_decision
#define zygisk_self_destruct fd_test_self_destruct
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "hook.cpp"
#undef zygisk_load_modules
#undef zygisk_run_app_pre
#undef zygisk_run_app_post
#undef zygisk_inject_decision
#undef zygisk_self_destruct
#include <cassert>
#include <fcntl.h>
#include <sys/stat.h>

namespace {
ZygiskContext fd_context;
}

void fixture_snapshot_fds() {
  fd_context.allowed_fds.assign(fd_table_size(), false);
  DIR *directory = opendir("/proc/self/fd");
  assert(directory);
  while (const dirent *entry = readdir(directory)) {
    const int fd = atoi(entry->d_name);
    if (fd != dirfd(directory))
      mark_allowed(&fd_context, fd);
  }
  closedir(directory);
}

int fixture_dependency_directory(const char *path) {
  struct stat expected{};
  assert(stat(path, &expected) == 0);
  DIR *directory = opendir("/proc/self/fd");
  assert(directory);
  int found = -1;
  while (const dirent *entry = readdir(directory)) {
    const int fd = atoi(entry->d_name);
    if (fd == dirfd(directory) || fd_context.allowed_fds.at(fd))
      continue;
    struct stat actual{};
    if (fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev &&
        actual.st_ino == expected.st_ino) {
      assert(found == -1);
      found = fd;
    }
  }
  closedir(directory);
  assert(found >= 0);
  return found;
}

void fixture_sanitize_fds(bool app) {
  jintArray ignored = nullptr;
  fd_context.pid = 0;
  fd_context.fds_to_ignore = app ? &ignored : nullptr;
  ctx_sanitize_fds(&fd_context);
  fd_context.fds_to_ignore = nullptr;
}

void fixture_quiesce_unused_hooks() {
  // This driver has no ART runtime. Establish the same quiescent state that
  // the device reaches after retiring its child hooks, before testing Core TSD.
  assert(!g_runtime_ready && !g_jni_hooked && g_jni_hooks.empty());
  assert(!g_fork_handle.valid() && !g_strdup_handle.valid() &&
         !g_art_handle.valid());
  assert(!g_orig_fork.load() && !g_strdup.load() &&
         !g_art_post_fork_child.load());
  assert(g_plt_callbacks.load(std::memory_order_acquire) == 0);
  g_fork_quiet = g_strdup_quiet = g_art_quiet = true;
}

namespace {
int payload_fd = -1;
unsigned callback_stage;
unsigned module_forks;

void check_preserved_fd(unsigned stage) {
  assert(callback_stage == stage);
  ++callback_stage;
  assert(zygisk_exempt_fd(payload_fd));
  assert(!zygisk_exempt_fd(-1));
  assert(g_ctx && g_ctx->exempted_fds.empty());
  char byte = 1;
  assert(read(payload_fd, &byte, 1) == 1 && byte == 0);
}

int module_fork_probe() {
  ++module_forks;
  return 12345;
}

template <typename... Args> void original_specialize(Args...) {
  check_preserved_fd(2);
  assert(new_fork() == 12345 && module_forks == 1);
}

template <typename... Args>
void exercise_specialize(ZygoteMethodIndex index, Args... args) {
  assert(g_ctx == nullptr && !g_orig_fork.load());
  payload_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
  assert(payload_fd >= 0 && !zygisk_exempt_fd(payload_fd));
  callback_stage = module_forks = 0;
  auto &method = g_zygote_methods[index];
  auto *wrapper = method.fnPtr;
  method.fnPtr = reinterpret_cast<void *>(original_specialize<Args...>);
  *g_orig_fork.output_address() = reinterpret_cast<void *>(module_fork_probe);
  reinterpret_cast<void (*)(Args...)>(wrapper)(args...);
  method.fnPtr = wrapper;
  *g_orig_fork.output_address() = nullptr;
  assert(callback_stage == 5 && g_ctx == nullptr);
  assert(!zygisk_exempt_fd(payload_fd));
  assert(close(payload_fd) == 0);
  payload_fd = -1;
}

struct TestIntArray {
  _jintArray handle;
  std::vector<jint> values;
};
TestIntArray initial_fds, merged_fds;

std::vector<jint> &array_values(jarray array) {
  if (array == &merged_fds.handle)
    return merged_fds.values;
  assert(array == &initial_fds.handle);
  return initial_fds.values;
}

void exercise_fork_fd_sweep() {
  // Preserve the driver's real descriptors; only fds opened below are new.
  fixture_snapshot_fds();
  const int ignored_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  const int exempted_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
  const int dropped_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  assert(ignored_fd >= 0 && exempted_fd >= 0 && dropped_fd >= 0);

  // Supply just the JNI array operations used by the production fd sweep.
  JNINativeInterface jni{};
  jni.GetArrayLength = [](JNIEnv *, jarray array) -> jsize {
    return static_cast<jsize>(array_values(array).size());
  };
  jni.GetIntArrayElements = [](JNIEnv *, jintArray array,
                               jboolean *) -> jint * {
    return array_values(array).data();
  };
  jni.ReleaseIntArrayElements = [](JNIEnv *, jintArray, jint *, jint) {};
  jni.NewIntArray = [](JNIEnv *, jsize size) -> jintArray {
    merged_fds.values.assign(static_cast<size_t>(size), 0);
    return &merged_fds.handle;
  };
  jni.SetIntArrayRegion = [](JNIEnv *, jintArray array, jsize start,
                             jsize count, const jint *values) {
    auto &dest = array_values(array);
    assert(start >= 0 && count >= 0 &&
           static_cast<size_t>(start + count) <= dest.size());
    std::copy(values, values + count, dest.begin() + start);
  };
  JNIEnv env{&jni};
  initial_fds.values.push_back(ignored_fd);
  jintArray ignored = &initial_fds.handle;
  ZygiskContext ctx;
  ctx.env = &env;
  ctx.pid = 0;
  ctx.allowed_fds = fd_context.allowed_fds;
  g_ctx = &ctx;
  // The server path has no exemption channel and must still reject requests.
  assert(!zygisk_exempt_fd(exempted_fd));
  ctx.fds_to_ignore = &ignored;
  assert(!zygisk_exempt_fd(-1));
  assert(zygisk_exempt_fd(exempted_fd));
  ctx_sanitize_fds(&ctx);
  g_ctx = nullptr;
  assert(ignored == &merged_fds.handle);
  assert((merged_fds.values == std::vector<jint>{ignored_fd, exempted_fd}));
  assert(fcntl(ignored_fd, F_GETFD) >= 0);
  char byte = 1;
  assert(read(exempted_fd, &byte, 1) == 1 && byte == 0);
  assert(fcntl(dropped_fd, F_GETFD) == -1 && errno == EBADF);
  assert(close(ignored_fd) == 0 && close(exempted_fd) == 0);
  std::vector<jint>().swap(initial_fds.values);
  std::vector<jint>().swap(merged_fds.values);
}
} // namespace

int fd_test_inject_decision(int uid) {
  assert(uid == 10000);
  return 0;
}
void fd_test_load_modules(JNIEnv *) { check_preserved_fd(0); }
void fd_test_run_app_pre(zygisk::AppSpecializeArgs *) { check_preserved_fd(1); }
void fd_test_run_app_post(JNIEnv *, const zygisk::AppSpecializeArgs *) {
  check_preserved_fd(3);
}
void fd_test_self_destruct(JNIEnv *, bool isolated) {
  assert(!isolated && callback_stage == 4);
  ++callback_stage;
  // Retirement may never return to the wrapper. Its context must be gone.
  assert(g_ctx == nullptr && !zygisk_exempt_fd(payload_fd));
  assert(fcntl(payload_fd, F_GETFD) >= 0);
}

void fixture_exempt_fds() {
  JNIEnv *env = nullptr;
  jclass clazz = nullptr;
  jint uid = 10000, gid = uid, flags = 0, mount = 0;
  jintArray gids = nullptr;
  jobjectArray limits = nullptr, data = nullptr;
  jstring text = nullptr;
  jboolean no = JNI_FALSE;
  exercise_specialize(kSpecializeLegacy, env, clazz, uid, gid, gids, flags,
                      limits, mount, text, text, no, text, text, no, data, data,
                      no, no);
  exercise_specialize(kSpecializeSysprop, env, clazz, uid, gid, gids, flags,
                      limits, mount, text, text, no, text, text, no, data, data,
                      no, no, no);
  exercise_specialize(kSpecializeCgroup, env, clazz, uid, uid, gid, gids, flags,
                      limits, mount, text, text, no, text, text, no, data, data,
                      no, no, no);
  exercise_fork_fd_sweep();
  puts("PASS exemptFd: three specialize signatures, callback lifetime, module "
       "fork dispatch, invalid requests and fork fd preservation/cleanup");
}
