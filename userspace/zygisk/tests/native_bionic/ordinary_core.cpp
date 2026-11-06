// Include the complete production translation unit to drive its internal module
// ownership and ABI callbacks without an ART process or kernel daemon.
#include "core.cpp"
#include "handoff_control.hpp"
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <sched.h>

namespace {
unsigned patch_number;
} // namespace

extern "C" uint8_t __wrap_yz_patch_text_checked(uintptr_t, const void *,
                                                const void *, unsigned) {
  ++patch_number;
  return 0;
}

extern "C" void yz_jni_entry_0();
extern "C" void *g_yz_jni_targets[];
extern "C" pid_t __real_fork();
void fixture_quiesce_unused_hooks();
void fixture_snapshot_fds();
int fixture_dependency_directory(const char *path);
void fixture_sanitize_fds(bool app);
void exercise_exit_cleanup();

extern "C" void __wrap_yz_log(uint8_t, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

namespace {
IntegrationControl *control;
extern "C" int __cxa_atexit(void (*)(void *), void *, void *);

void foreign_exit_count(void *count) { ++*static_cast<unsigned *>(count); }
int exercise_exit_lifecycle(const char *directory) {
  using namespace yukizygisk;
  exercise_exit_cleanup();
  char path[4096];
  snprintf(path, sizeof(path), "%s/libintegration_missing.so", directory);
  const auto constructors = control->module_constructors;
  const auto finalizers = control->module_finalizers;
  const auto patches = patch_number;
  const auto *registration = reinterpret_cast<const void *>(
      reinterpret_cast<uintptr_t>(__cxa_atexit) & ~uintptr_t{1});
  const auto *finalization = reinterpret_cast<const void *>(
      reinterpret_cast<uintptr_t>(__cxa_finalize) & ~uintptr_t{1});
  uint8_t saved_register[16], saved_finalize[16];
  memcpy(saved_register, registration, sizeof(saved_register));
  memcpy(saved_finalize, finalization, sizeof(saved_finalize));
  const auto load = [&] {
    linker::Library library;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 && library.open(fd, path, -1, true, false, false));
    close(fd);
    return library;
  };
  for (int repeat = 0; repeat != 3; ++repeat) {
    auto library = load();
    assert(library.close());
  }
  assert(control->module_constructors == constructors + 3 &&
         control->module_finalizers == finalizers + 3);
  assert(exit_cleanup::compact().status == exit_cleanup::Status::Complete);

  auto library = load();
  unsigned foreign = 0;
  assert(__cxa_atexit(foreign_exit_count, &foreign, &foreign) == 0);
  int channels[2];
  assert(pipe(channels) == 0);
  pthread_t thread;
  assert(pthread_create(
             &thread, nullptr,
             [](void *opaque) -> void * {
               char byte;
               assert(read(*static_cast<int *>(opaque), &byte, 1) == 1);
               return nullptr;
             },
             &channels[0]) == 0);
  // Specialization can start a thread before post callbacks. Closing its last
  // image and compacting exit entries must remain safe with that thread alive.
  assert(library.retained());
  assert(library.close());
  assert(exit_cleanup::compact().status == exit_cleanup::Status::Complete);
  assert(control->module_constructors == constructors + 4 &&
         control->module_finalizers == finalizers + 4);
  __cxa_finalize(&foreign);
  assert(foreign == 1 && patch_number == patches);
  assert(memcmp(saved_register, registration, sizeof(saved_register)) == 0 &&
         memcmp(saved_finalize, finalization, sizeof(saved_finalize)) == 0);
  assert(write(channels[1], "x", 1) == 1 && pthread_join(thread, nullptr) == 0);
  close(channels[0]);
  close(channels[1]);
  control->module_constructors = constructors;
  control->module_finalizers = finalizers;
  puts("PASS production exit lifecycle: foreign registration, live thread, "
       "finalizers and no libc entry patches");
  return 0;
}
yukiplt::OriginalSlot<int (*)()> original;
int replacement() { return original.load()() + 7; }
int fork_fixture() {
  return yukizygisk::plt_lifecycle::fork_process(__real_fork);
}
int retire_fixture(void *) {
  // An unrelated live Core key must still defer retirement after SDK shutdown.
  pthread_key_t retained_key;
  assert(pthread_key_create(&retained_key, nullptr) == 0);
  zygisk_self_destruct(nullptr, false);
  assert(g_core_retiring && !core_runtime_closed() &&
         !yukizygisk::linker::ready());
  assert(pthread_key_delete(retained_key) == 0);
  zygisk_self_destruct(nullptr, false);
  return -1;
}
void reuse_fd(int fd, const char *path) {
  assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  const int replacement = open(path, O_RDONLY | O_CLOEXEC);
  assert(replacement >= 0);
  if (replacement != fd) {
    assert(dup2(replacement, fd) == fd);
    close(replacement);
  }
}
void run_pre(bool app) {
  jint uid = app ? 10000 : 1000, gid = uid, flags = 0, mount = 0;
  jintArray gids = nullptr;
  if (app) {
    jobjectArray limits = nullptr;
    jstring text = nullptr;
    zygisk::AppSpecializeArgs args{uid,   gid,  gids, flags, limits,
                                   mount, text, text, text,  text};
    zygisk_run_app_pre(&args);
  } else {
    jlong permitted = 0, effective = 0;
    zygisk::ServerSpecializeArgs args{uid,   gid,       gids,
                                      flags, permitted, effective};
    zygisk_run_server_pre(&args);
  }
}
int exercise_adapter(const char *directory) {
  using namespace yukizygisk;
  plt_lifecycle::ExternalScope external;
  assert(external);
  // The anonymous Core cannot see this registered host provider on its own.
  assert(dlsym(RTLD_DEFAULT, "integration_system_probe") == nullptr);
  dlerror();
  fixture_snapshot_fds();
  char path[4096];
  snprintf(path, sizeof(path), "%s/libintegration_private.so", directory);
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(fd >= 0);
  const int dependencies = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(dependencies >= 0);
  linker::Library library;
  assert(library.open(fd, path, dependencies, true, false, false));
  close(dependencies);
  close(fd);
  assert(library.is_private());
  const int retained_directory = fixture_dependency_directory(directory);
  run_pre(false);
  fixture_sanitize_fds(false);
  char alias_directory[4096];
  const int directory_length =
      snprintf(alias_directory, sizeof(alias_directory), "%s/fd-reuse-XXXXXX",
               directory);
  assert(directory_length > 0 &&
         static_cast<size_t>(directory_length) < sizeof(alias_directory));
  assert(mkdtemp(alias_directory));
  char alias[4096];
  const int alias_length = snprintf(
      alias, sizeof(alias), "%s/libintegration_fd_alias.so", alias_directory);
  assert(alias_length > 0 && static_cast<size_t>(alias_length) < sizeof(alias));
  assert(symlink(path, alias) == 0);
  reuse_fd(retained_directory, alias_directory);
  auto open_module =
      reinterpret_cast<int (*)(const char *)>(library.symbol("module_open"));
  assert(open_module);
  {
    linker::Execution execution(library);
    assert(execution && !open_module("libintegration_fd_alias.so"));
  }
  assert(linker::private_images().size() == 2);
  auto probe = reinterpret_cast<int (*)()>(library.symbol("module_probe"));
  auto call = reinterpret_cast<int (*)()>(library.symbol("module_pid"));
  auto fork_module = reinterpret_cast<int (*)()>(library.symbol("module_fork"));
  auto thread =
      reinterpret_cast<void *(*)(void *)>(library.symbol("module_thread"));
  assert(probe && call && fork_module && thread);
  unsigned running = 1;
  pthread_t worker;
  {
    linker::Execution execution(library);
    assert(execution && probe() && call() == getpid());
    assert(fork_module() == 0);
    assert(pthread_create(&worker, nullptr, thread, &running) == 0);
    while (!__atomic_load_n(&control->worker_ready, __ATOMIC_ACQUIRE))
      sched_yield();
    {
      plt_lifecycle::Operation operation;
      assert(operation);
      auto pin = linker::private_image(reinterpret_cast<void *>(probe));
      assert(pin && pin->named.image.instance_token);
      assert(plt_lifecycle::capture_maps().ok());
      const yukiplt::HookRequest request{"getpid",
                                         reinterpret_cast<void *>(replacement),
                                         original.output_address()};
      yukiplt::SlotReport slot;
      yukiplt::OperationDiagnostic diagnostic;
      auto hooked = linker::hook_batch(plt_lifecycle::maps_cache(), pin->named,
                                       &request, 1, &slot, &diagnostic);
      assert(hooked.ok() && call() == getpid() + 7);
      assert(plt_lifecycle::unhook(hooked.handle, &slot, 1, &diagnostic).ok());
      assert(call() == getpid());
      assert(!library.close() && library.retained());
      assert(probe());
    }
  }
  assert(!library.close() && control->module_finalizers == 0);
  __atomic_store_n(&running, 0U, __ATOMIC_RELEASE);
  assert(pthread_join(worker, nullptr) == 0);
  assert(control->thread_destructors == 1);
  assert(library.close() && library.close());
  assert(fcntl(retained_directory, F_GETFD) >= 0);
  close(retained_directory);
  assert(unlink(alias) == 0 && rmdir(alias_directory) == 0);
  assert(control->module_constructors == 1 && control->module_finalizers == 1);
  fixture_snapshot_fds();
  snprintf(path, sizeof(path), "%s/libintegration_emutls.so", directory);
  const int fallback_fd = open(path, O_RDONLY | O_CLOEXEC);
  assert(fallback_fd >= 0);
  const int fallback_directory =
      open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(fallback_directory >= 0);
  linker::Library fallback;
  assert(
      fallback.open(fallback_fd, path, fallback_directory, true, false, true));
  close(fallback_directory);
  close(fallback_fd);
  const int fallback_retained = fixture_dependency_directory(directory);
  run_pre(true);
  fixture_sanitize_fds(true);
  reuse_fd(fallback_retained, "/dev/null");
  assert(!fallback.is_private() && fallback.symbol("module_pid"));
  assert(fallback.close());
  assert(fcntl(fallback_retained, F_GETFD) >= 0);
  close(fallback_retained);
  snprintf(path, sizeof(path), "%s/libintegration_missing.so", directory);
  const int missing_fd = open(path, O_RDONLY | O_CLOEXEC);
  linker::Library missing;
  assert(missing_fd >= 0 &&
         missing.open(missing_fd, path, -1, true, true, false));
  close(missing_fd);
  assert(!missing.symbol("zn_module") && missing.close());
  puts("PASS adapter PRIVATE TLS/PHDR/fork/hooks/close and SYSTEM fallback");
  puts(
      "PASS app/server pre-specialize revokes dependency access and ownership");
  return 0;
}
int exercise_ordinary(const char *directory) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/libintegration_private.so", directory);
  const int dependencies = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(dependencies >= 0);
  fixture_snapshot_fds();
  g_modules.reserve(1);
  for (unsigned rejected : {1U, 0U}) {
    yukizygisk::plt_lifecycle::ExternalScope external;
    assert(external);
    control->reject_registration = rejected;
    auto &module = g_modules.emplace_back();
    module.id = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 &&
           module.library.open(fd, path, dependencies, true, false, true));
    close(fd);
    assert(module.library.is_private());
    auto entry = reinterpret_cast<module_entry_fn>(
        module.library.symbol("zygisk_module_entry"));
    assert(entry);
    module.api.registerModule = RegisterModuleImpl;
    g_loading = &module;
    g_loading_id = 0;
    {
      ModuleCallbackScope callback(module);
      assert(callback);
      entry(reinterpret_cast<api_table *>(&module.api), nullptr);
    }
    g_loading = nullptr;
    if (rejected) {
      assert(module.version == 0 && module.library.close());
      g_modules.pop_back();
    } else {
      assert(module.version == ZYGISK_API_VERSION);
    }
  }
  jint uid = 10000, gid = 10000, flags = 0, mount = 0;
  jintArray gids = nullptr;
  jobjectArray limits = nullptr;
  jstring text = nullptr;
  zygisk::AppSpecializeArgs app{uid,   gid,  gids, flags, limits,
                                mount, text, text, text,  text};
  const int retained_directory = fixture_dependency_directory(directory);
  zygisk_run_app_pre(&app);
  // USAP enters specialize without the fork path's general fd sweep.
  reuse_fd(retained_directory, "/dev/null");
  assert(fcntl(dependencies, F_GETFD) >= 0);
  zygisk_run_app_post(nullptr, &app);
  assert(control->app_pre == 1 && control->app_post == 1);
  assert(!zygisk_app_core_unload_safe());
  jlong permitted = 0, effective = 0;
  zygisk::ServerSpecializeArgs server{uid,   gid,       gids,
                                      flags, permitted, effective};
  zygisk_run_server_pre(&server);
  zygisk_run_server_post(nullptr, &server);
  assert(control->server_pre == 1 && control->server_post == 1);
  assert(control->ordinary_entries == 2 && control->module_constructors == 2 &&
         control->module_finalizers == 2);
  assert(zygisk_app_core_unload_safe());
  assert(fcntl(retained_directory, F_GETFD) >= 0 &&
         fcntl(dependencies, F_GETFD) >= 0);
  close(retained_directory);
  close(dependencies);
  puts("PASS ordinary registration refusal, app/server callbacks, residency "
       "and explicit unload");
  puts("PASS USAP pre-specialize closes only Core dependency directories");
  return 0;
}
int exercise(const char *directory) {
  control->base = g_self_base;
  control->span = g_self_size;
  const int result = control->adapter_only ? exercise_adapter(directory)
                                           : exercise_ordinary(directory);
  fixture_quiesce_unused_hooks();
  g_yz_jni_targets[0] = reinterpret_cast<void *>(retire_fixture);
  control->retire = reinterpret_cast<int (*)(void *)>(yz_jni_entry_0);
  return result;
}
__attribute__((constructor)) void initialize_fixture() {
  control = reinterpret_cast<IntegrationControl *>(
      strtoull(getenv("YUKISU_INTEGRATION_CONTROL"), nullptr, 16));
  control->exercise = exercise;
  control->exercise_exit_lifecycle = exercise_exit_lifecycle;
  control->fork_process = fork_fixture;
}
} // namespace
