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
#include <time.h>
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
bool exercise_keys = false;
pthread_key_t inherited_key;
std::atomic<bool> pause_key{false}, key_entered{false}, release_key{false};
std::atomic<bool> pause_restore{false}, restore_entered{false},
    release_restore{false};
std::atomic<bool> watch_drain{false}, drain_entered{false};
std::atomic<bool> snapshot_entered{false}, snapshot_boundary_done{false};
bool pause_snapshot = false;
std::atomic<bool> fork_entered{false};

void wait_for(const std::atomic<bool> &flag) {
  while (!flag.load(std::memory_order_acquire))
    std::this_thread::yield();
}

void probe_key() {
  pthread_key_t key;
  CHECK(pthread_key_create(&key, nullptr) == 0);
  CHECK(pthread_key_delete(key) == 0);
}

void prepare_atfork() {
  if (!exercise_keys)
    return;
  probe_key();
  CHECK(pthread_key_create(&inherited_key, nullptr) == 0);
  CHECK(pthread_setspecific(inherited_key, &inherited_key) == 0);
}

void complete_atfork() {
  if (!exercise_keys)
    return;
  CHECK(pthread_getspecific(inherited_key) == &inherited_key);
  CHECK(pthread_key_delete(inherited_key) == 0);
  probe_key();
}

void child_atfork() {
  complete_atfork();
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

template <typename F> void in_child(F body) {
  const pid_t child = yukizygisk::plt_lifecycle::fork_process(fork);
  CHECK(child >= 0);
  if (child == 0) {
    alarm(10);
    body();
    _exit(0);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
}

pid_t fork_with_pending_boundary() {
  drain_entered.store(false);
  watch_drain.store(true);
  fork_entered.store(true, std::memory_order_release);
  wait_for(drain_entered);
  return fork();
}

pid_t fork_with_snapshot_boundary() {
  const pid_t result = fork();
  if (result == 0) {
    pause_snapshot = true;
    watch_drain.store(true);
    drain_entered.store(false);
    std::thread([] {
      wait_for(snapshot_entered);
      {
        yukizygisk::plt_lifecycle::ExternalScope boundary;
        CHECK(boundary);
      }
      snapshot_boundary_done.store(true, std::memory_order_release);
    }).detach();
  }
  return result;
}

pthread_key_t draining_key;
int destructor_calls = 0;
void drain_destructor(void *) {
  CHECK(pthread_key_delete(draining_key) == 0);
  if (++destructor_calls == 1) {
    CHECK(pthread_key_create(&draining_key, drain_destructor) == 0);
    CHECK(pthread_setspecific(draining_key, &draining_key) == 0);
  }
}

void check_fork_boundaries(PltFixture &fixture,
                           const yukiplt::HookRequest &request) {
  using namespace yukizygisk::plt_lifecycle;
  exercise_keys = true;
  in_child([] {
    std::thread fork_thread([] {
      ExternalScope outer;
      CHECK(outer);
      in_child([] {
        ExternalScope nested;
        CHECK(nested);
        Operation operation;
        CHECK(operation);
        probe_key();
      });
    });
    fork_thread.join();
  });

  pause_key.store(true);
  pthread_key_t worker_key;
  std::thread key_worker(
      [&] { CHECK(pthread_key_create(&worker_key, nullptr) == 0); });
  wait_for(key_entered);
  CHECK(fork_process(failed_fork) == -1 && errno == EBUSY);
  release_key.store(true, std::memory_order_release);
  key_worker.join();
  CHECK(pthread_key_delete(worker_key) == 0);
  CHECK(fork_process(failed_fork) == -1 && errno == EAGAIN);
  probe_key();

  CHECK(pthread_key_create(&worker_key, nullptr) == 0);
  CHECK(core_prepare_fork());
  std::thread denied([&] {
    pthread_key_t output = worker_key;
    CHECK(pthread_key_create(&output, nullptr) == EAGAIN &&
          output == worker_key);
    CHECK(pthread_key_delete(worker_key) == EBUSY);
    CHECK(core_private_tsd_used());
  });
  denied.join();
  probe_key();
  core_after_fork_parent();
  CHECK(pthread_key_delete(worker_key) == 0);
  CHECK(pthread_key_create(&draining_key, drain_destructor) == 0);
  CHECK(pthread_setspecific(draining_key, &draining_key) == 0);
  CHECK(core_drain_current_thread_tsd() == 0 && destructor_calls == 2);

  std::thread pending([] {
    wait_for(fork_entered);
    ExternalScope boundary;
    CHECK(boundary);
  });
  pid_t child = fork_process(fork_with_pending_boundary);
  CHECK(child >= 0);
  if (child == 0) {
    ExternalScope boundary;
    CHECK(boundary);
    Operation operation;
    CHECK(operation);
    _exit(0);
  }
  pending.join();
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);

  yukiplt::SlotReport slot;
  yukiplt::OperationDiagnostic diagnostic;
  auto installed = yukiplt::hook_batch_nonnull_diagnosed_named(
      fixture.image(), &request, 1, &slot, &diagnostic);
  CHECK(installed.ok());
  in_child([&] {
    std::atomic<bool> restored{false};
    pause_restore.store(true);
    std::thread restore([&] {
      Operation operation;
      CHECK(operation);
      CHECK(unhook(installed.handle, &slot, 1, &diagnostic).ok());
      restored.store(true, std::memory_order_release);
    });
    wait_for(restore_entered);
    drain_entered.store(false);
    watch_drain.store(true);
    std::thread boundary([&] {
      ExternalScope external;
      CHECK(external && restored.load(std::memory_order_acquire));
      ExternalScope nested;
      CHECK(nested);
      const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
      void *address = reinterpret_cast<void *>(slot.address & ~(page - 1));
      CHECK(mprotect(address, page, PROT_READ | PROT_WRITE) == 0);
      CHECK(mprotect(address, page, PROT_READ) == 0);
      Operation operation;
      CHECK(operation && capture_maps().ok());
      CHECK(fixture.call() == 17);
    });
    wait_for(drain_entered);
    {
      Operation blocked;
      CHECK(!blocked);
    }
    release_restore.store(true, std::memory_order_release);
    restore.join();
    boundary.join();
  });

  child = fork_process(fork_with_snapshot_boundary);
  CHECK(child >= 0);
  if (child == 0) {
    alarm(10);
    wait_for(snapshot_boundary_done);
    Operation operation;
    CHECK(operation);
    fail_maps = true;
    CHECK(!unhook(installed.handle, &slot, 1, &diagnostic).ok());
    CHECK(installed.handle.valid());
    _exit(0);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);

  in_child([&] {
    std::atomic<bool> entered{false}, release{false};
    std::thread active([&] {
      Operation operation;
      CHECK(operation);
      entered.store(true, std::memory_order_release);
      wait_for(release);
    });
    wait_for(entered);
    ExternalScope timeout(1);
    CHECK(!timeout && timeout.error() == ETIMEDOUT && !dispatch_allowed());
    release.store(true, std::memory_order_release);
    active.join();
    ModuleCallbackScope callback(g_modules[0]);
    CHECK(!callback && g_cur == nullptr && g_plt_callback_tid.load() == 0);
    Operation recovery(Access::Recover);
    CHECK(!recovery && !shutdown());
    CHECK(installed.handle.valid() && fixture.call() == 117);
    CHECK(fork_process(failed_fork) == -1 && errno == EAGAIN);
    in_child([] { CHECK(!dispatch_allowed()); });
  });
  in_child([] {
    Operation operation;
    CHECK(operation);
    ExternalScope recursive;
    CHECK(!recursive && recursive.error() == EDEADLK && !dispatch_allowed());
  });
  in_child([] {
    std::atomic<bool> entered{false}, release{false};
    std::thread owner([&] {
      ExternalScope external;
      CHECK(external);
      entered.store(true, std::memory_order_release);
      wait_for(release);
    });
    wait_for(entered);
    ExternalScope competing;
    CHECK(!competing && competing.error() == EBUSY && !dispatch_allowed());
    release.store(true, std::memory_order_release);
    owner.join();
  });
  CHECK(yukiplt::unhook(installed.handle).ok());
  exercise_keys = false;
  puts("PASS: atfork key reentry, key admission, nested boundaries, drain "
       "ordering, "
       "snapshot invalidation and failure retention");
}
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
  if (pause_restore.exchange(false)) {
    restore_entered.store(true, std::memory_order_release);
    wait_for(release_restore);
  }
  if (fail_restore && protection == PROT_READ) {
    fail_restore = false;
    errno = EACCES;
    return -1;
  }
  return __real_mprotect(address, size, protection);
}
extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...) {
  if (pause_snapshot && strcmp(path, "/proc/self/maps") == 0) {
    pause_snapshot = false;
    snapshot_entered.store(true, std::memory_order_release);
    wait_for(drain_entered);
  }
  if (fail_maps && strcmp(path, "/proc/self/maps") == 0) {
    errno = EACCES;
    return -1;
  }
  return __real_open(path, flags, 0);
}

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (pause_key.exchange(false)) {
    key_entered.store(true, std::memory_order_release);
    wait_for(release_key);
  }
  return __real_pthread_mutex_lock(mutex);
}

extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec *now) {
  if (clock == CLOCK_MONOTONIC && watch_drain.exchange(false))
    drain_entered.store(true, std::memory_order_release);
  return __real_clock_gettime(clock, now);
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
  CHECK(pthread_atfork(prepare_atfork, complete_atfork, child_atfork) == 0);
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
    yukizygisk::plt_lifecycle::invalidate_fork_cache();
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
  check_fork_boundaries(fixture, request);
  {
    yukizygisk::plt_lifecycle::Operation operation;
    CHECK(operation && yukizygisk::plt_lifecycle::shutdown());
    CHECK(!core_private_tsd_used());
  }
  puts("PASS: module ownership, failure isolation, cached child restoration, "
       "multithreaded fork and dispatcher retirement");
  return 0;
}
