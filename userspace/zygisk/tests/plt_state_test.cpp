#include "core_bindings.hpp"
#include "plt_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <regex.h>
#include <thread>
namespace {
yukiplt::OriginalSlot<int (*)()> original;
int replacement() { return original.load()() + 100; }
int foreign() { return 999; }
int other_replacement() { return 42; }
pid_t failed_fork() {
  errno = EAGAIN;
  return -1;
}
} // namespace
namespace {
bool fail_maps = false;
bool fail_allocation = false;
bool fail_restore = false;
void *child_readonly_page = nullptr;
size_t child_page_size = 0;
void child_atfork() {
  if (child_readonly_page != nullptr &&
      mprotect(child_readonly_page, child_page_size, PROT_READ) != 0)
    _exit(2);
}
#include "module_plt_types.inc"
struct Module {
  int id = 0;
  std::vector<ModulePltRequest> plt_pending;
  std::vector<std::unique_ptr<ModulePltGroup>> plt_groups;
  bool plt_registration_failed = false;
};
std::vector<Module> g_modules;
Module *g_cur = nullptr;
std::atomic<pid_t> g_plt_callback_tid{0};
bool zygisk_original_output_allowed(void **) { return true; }
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#include "module_plt_methods.inc"
} // namespace
#if UINTPTR_MAX == UINT64_MAX
extern "C" void *__real__Znwm(size_t);
extern "C" void *__wrap__Znwm(size_t size) {
  if (fail_allocation) {
    fail_allocation = false;
    throw std::bad_alloc();
  }
  return __real__Znwm(size);
}
#else
extern "C" void *__real__Znwj(size_t);
extern "C" void *__wrap__Znwj(size_t size) {
  if (fail_allocation) {
    fail_allocation = false;
    throw std::bad_alloc();
  }
  return __real__Znwj(size);
}
#endif
extern "C" int __real_mprotect(void *, size_t, int);
extern "C" int __wrap_mprotect(void *address, size_t size, int protection) {
  if (fail_restore && protection == PROT_READ) {
    fail_restore = false;
    errno = EACCES;
    return -1;
  }
  return __real_mprotect(address, size, protection);
}
extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...) {
  if (fail_maps && strcmp(path, "/proc/self/maps") == 0) {
    errno = EACCES;
    return -1;
  }
  return __real_open(path, flags, 0);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  PltFixture fixture(argv[1]);
  const yukiplt::HookRequest request{"fixture_value",
                                     reinterpret_cast<void *>(replacement),
                                     original.output_address()};
  yukiplt::OperationDiagnostic diagnostic;
  yukiplt::SlotReport slot;
  auto result = yukiplt::hook_batch_nonnull_diagnosed_named(
      fixture.image(), &request, 1, &slot, &diagnostic);
  CHECK(result.ok() && result.handle.valid() && fixture.call() == 117);
  write_fixture_slot(slot.address, reinterpret_cast<void *>(foreign));
  CHECK(!yukiplt::unhook_diagnosed(result.handle, &slot, 1, &diagnostic).ok());
  CHECK(result.handle.valid() && fixture.call() == 999);
  write_fixture_slot(slot.address, reinterpret_cast<void *>(replacement));
  CHECK(yukiplt::unhook_diagnosed(result.handle, &slot, 1, &diagnostic).ok());
  CHECK(!result.handle.valid() && diagnostic.handle_consumed &&
        fixture.call() == 17);
  {
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation);
  }
  std::atomic<bool> ready{false}, done{false};
  std::thread worker([&] {
    ready.store(true);
    while (!done.load())
      std::this_thread::yield();
  });
  while (!ready.load())
    std::this_thread::yield();
  {
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation && yukizygisk::plt_lifecycle::capture_maps().ok());
    result = yukiplt::hook_batch_nonnull_named_cached(
        yukizygisk::plt_lifecycle::maps_cache(), fixture.image(), &request, 1,
        &slot, &diagnostic);
    CHECK(result.ok());
    child_page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    child_readonly_page = reinterpret_cast<void *>(
        slot.address & ~(static_cast<uintptr_t>(child_page_size) - 1));
    CHECK(yukizygisk::plt_lifecycle::maps_cache().invalidate().ok());
    CHECK(mprotect(child_readonly_page, child_page_size,
                   PROT_READ | PROT_WRITE) == 0);
    CHECK(yukizygisk::plt_lifecycle::capture_maps().ok());
  }
  CHECK(pthread_atfork(nullptr, nullptr, child_atfork) == 0);
  pid_t child = yukizygisk::plt_lifecycle::fork_process(fork);
  CHECK(child >= 0);
  if (child == 0) {
    alarm(10);
    fail_maps = true;
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation);
    yukiplt::ForkStatus state{sizeof(state)};
    CHECK(yukiplt::fork_status(state).ok() &&
          state.mode == YUKIPLT_C_FORK_HOST_DISPATCHER &&
          state.phase == YUKIPLT_C_FORK_READY && state.generation == 1);
    size_t count = 0;
    CHECK(yukiplt::query_cached(yukizygisk::plt_lifecycle::maps_cache(),
                                result.handle, &slot, 1, &count)
              .ok() &&
          count == 1);
    CHECK(
        yukizygisk::plt_lifecycle::unhook(result.handle, &slot, 1, &diagnostic)
            .ok());
    CHECK(!result.handle.valid() && fixture.call() == 17);
    CHECK(core_private_tsd_used());
    pthread_key_t key;
    CHECK(pthread_key_create(&key, nullptr) == 0);
    CHECK(pthread_key_delete(key) == 0);
    CHECK(yukizygisk::plt_lifecycle::shutdown());
    CHECK(!core_private_tsd_used());
    _exit(0);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  CHECK(result.handle.valid() && fixture.call() == 117);
  CHECK(mprotect(child_readonly_page, child_page_size, PROT_READ) == 0);
  child_readonly_page = nullptr;
  CHECK(yukizygisk::plt_lifecycle::fork_process(failed_fork) == -1 &&
        errno == EAGAIN);
  CHECK(yukiplt::unhook(result.handle).ok());
  done.store(true);
  worker.join();
  result = yukiplt::hook_batch_nonnull_diagnosed_named(
      fixture.image(), &request, 1, &slot, &diagnostic);
  CHECK(result.ok());
  child = yukizygisk::plt_lifecycle::fork_process(fork);
  CHECK(child >= 0);
  if (child == 0) {
    alarm(10);
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation);
    yukizygisk::plt_lifecycle::end_fork_window();
    fail_maps = true;
    CHECK(
        !yukizygisk::plt_lifecycle::unhook(result.handle, &slot, 1, &diagnostic)
             .ok() &&
        result.handle.valid());
    fail_maps = false;
    CHECK(
        yukizygisk::plt_lifecycle::unhook(result.handle, &slot, 1, &diagnostic)
            .ok());
    _exit(0);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  CHECK(yukiplt::unhook(result.handle).ok());
  child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    yukizygisk::plt_lifecycle::Operation operation;
    _exit(operation ? 1 : 0);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  yukiplt::OriginalSlot<int (*)()> recovery_original;
  const yukiplt::HookRequest recovery_request{
      "fixture_value", reinterpret_cast<void *>(other_replacement),
      recovery_original.output_address()};
  fail_maps = true;
  auto denied = yukiplt::hook_batch_nonnull_diagnosed_named(
      fixture.image(), &recovery_request, 1, &slot, &diagnostic);
  fail_maps = false;
  CHECK(!denied.ok() && !denied.handle.valid() &&
        recovery_original.load() == nullptr);
  fail_restore = true;
  auto partial = yukiplt::hook_batch_nonnull_diagnosed_named(
      fixture.image(), &recovery_request, 1, &slot, &diagnostic);
  CHECK(partial.error.code == yukiplt::ErrorCode::kIndeterminate &&
        partial.handle.valid());
  yukiplt::RecoveryDiagnostic recovery;
  CHECK(yukiplt::query_recovery(partial.handle, &recovery).ok() &&
        recovery.residual_page_count != 0);
  CHECK(yukizygisk::plt_lifecycle::fork_process(failed_fork) == -1 &&
        errno == EBUSY);
  CHECK(yukiplt::unhook(partial.handle).ok() && !partial.handle.valid() &&
        fixture.call() == 17);
  {
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation);
    CHECK(yukizygisk::plt_lifecycle::fork_process(failed_fork) == -1 &&
          errno == EBUSY);
  }
  g_modules.resize(2);
  g_cur = &g_modules[0];
  g_plt_callback_tid.store(gettid());
  api_plt_hook_register(fixture.map.dev, fixture.map.inode, nullptr,
                        reinterpret_cast<void *>(other_replacement), nullptr);
  CHECK(!api_plt_hook_commit() && api_plt_hook_commit());
  void *module_original = nullptr;
  api_plt_hook_register(fixture.map.dev, fixture.map.inode, "fixture_value",
                        reinterpret_cast<void *>(other_replacement),
                        &module_original);
  g_cur = &g_modules[1];
  CHECK(api_plt_hook_commit() && fixture.call() == 17);
  g_cur = &g_modules[0];
  CHECK(api_plt_hook_commit() && fixture.call() == 42);
  CHECK(!module_plt_can_unload(*g_cur));
  CHECK(yukiplt::unhook(g_cur->plt_groups[0]->handle).ok());
  CHECK(!module_plt_can_unload(*g_cur));
  void *oom_original = nullptr;
  api_plt_hook_register(fixture.map.dev, fixture.map.inode, "fixture_second",
                        reinterpret_cast<void *>(other_replacement),
                        &oom_original);
  fail_allocation = true;
  CHECK(!api_plt_hook_commit() && !fail_allocation && oom_original == nullptr);
  CHECK(g_cur->plt_groups.size() == 1 && fixture.call() == 17);
  void *failed_original = nullptr;
  {
    ModuleCallbackScope callback(g_modules[0]);
    api_plt_hook_register(fixture.map.dev, fixture.map.inode, "fixture_second",
                          reinterpret_cast<void *>(other_replacement),
                          &failed_original);
    fail_restore = true;
    CHECK(!api_plt_hook_commit());
    CHECK(g_modules[0].plt_groups.back()->handle.valid());
  }
  CHECK(failed_original != nullptr &&
        !g_modules[0].plt_groups.back()->handle.valid());
  CHECK(g_cur == nullptr && g_plt_callback_tid.load() == 0);
  g_plt_callback_tid.store(0);
  g_cur = nullptr;
  CHECK(!api_plt_hook_commit());
  {
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation && yukizygisk::plt_lifecycle::shutdown());
    CHECK(!core_private_tsd_used());
  }
  puts("PASS: module ownership, failure isolation, cached child restoration, "
       "multithreaded fork and dispatcher retirement");
  return 0;
}
