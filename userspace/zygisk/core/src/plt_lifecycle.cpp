#include "plt_lifecycle.hpp"
#include "core_bindings.hpp"

#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <unistd.h>

namespace yukizygisk::plt_lifecycle {
namespace {
std::atomic_flag operation_busy = ATOMIC_FLAG_INIT;
std::atomic<pid_t> owner_pid{0};
size_t pending_recoveries = 0;
bool sealed = false;
yukiplt::MapsCache cache;
std::atomic<bool> child_cache_ready{false};
yukiplt_c_fork_callbacks fork_callbacks{};
uint64_t subscription = 0;
uint64_t next_subscription = 1;

// Only fork_process selects callbacks, under Operation through completion.
// No callback address is registered with libc or outlives the Core's entries.
int subscribe(void *, const yukiplt_c_fork_callbacks *callbacks,
              uint64_t *token) noexcept {
  if (subscription != 0 || next_subscription == 0)
    return EBUSY;
  fork_callbacks = *callbacks;
  subscription = next_subscription++;
  *token = subscription;
  return 0;
}

int unsubscribe(void *, uint64_t token) noexcept {
  if (token == 0 || token != subscription)
    return EINVAL;
  fork_callbacks = {};
  subscription = 0;
  return 0;
}

bool single_threaded() noexcept {
  DIR *directory = opendir("/proc/self/task");
  if (directory == nullptr)
    return false;
  size_t count = 0;
  errno = 0;
  while (const dirent *entry = readdir(directory)) {
    if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9')
      ++count;
  }
  const int error = errno;
  const int closed = closedir(directory);
  return error == 0 && closed == 0 && count == 1;
}
} // namespace

Operation::Operation(Access access) noexcept {
  const pid_t current = getpid();
  pid_t empty = 0;
  owner_pid.compare_exchange_strong(empty, current, std::memory_order_acq_rel);
  if (owner_pid.load(std::memory_order_acquire) != current ||
      operation_busy.test_and_set(std::memory_order_acquire))
    return;
  if ((sealed && access != Access::Retire) ||
      (access == Access::Install && pending_recoveries != 0)) {
    operation_busy.clear(std::memory_order_release);
    return;
  }
  acquired_ = true;
}

void Operation::seal() const noexcept {
  if (acquired_)
    sealed = true;
}

Operation::~Operation() {
  if (acquired_)
    operation_busy.clear(std::memory_order_release);
}

void set_recovery(bool &owner_pending, bool pending) noexcept {
  if (owner_pending == pending)
    return;
  if (pending)
    ++pending_recoveries;
  else
    --pending_recoveries;
  owner_pending = pending;
}

bool recovery_pending() noexcept { return pending_recoveries != 0; }

bool retirement_window_ready(bool private_tsd_live) noexcept {
  return getpid() == gettid() &&
         owner_pid.load(std::memory_order_acquire) == getpid() &&
         (!private_tsd_live || single_threaded());
}

yukiplt::MapsCache &maps_cache() noexcept { return cache; }

yukiplt::Error capture_maps() noexcept {
  end_fork_window();
  return cache.capture();
}

void end_fork_window() noexcept {
  child_cache_ready.store(false, std::memory_order_release);
}

yukiplt::Error unhook(yukiplt::HookHandle &handle, yukiplt::SlotReport *slots,
                      size_t count, yukiplt::OperationDiagnostic *diagnostic) {
  if (!child_cache_ready.load(std::memory_order_acquire))
    return yukiplt::unhook_diagnosed(handle, slots, count, diagnostic);
  const auto error =
      yukiplt::unhook_cached(cache, handle, slots, count, diagnostic);
  if (!error.ok())
    end_fork_window();
  return error;
}

bool shutdown() noexcept {
  if (cache.has_bindings() || !yukiplt::disable_atfork().ok())
    return false;
  end_fork_window();
  cache = {};
  return true;
}

pid_t fork_process(pid_t (*original)()) noexcept {
  Operation operation;
  if (!operation || original == nullptr) {
    errno = EBUSY;
    return -1;
  }
  const yukiplt::ForkConfig config{
      sizeof(config), YUKIPLT_C_FORK_HOST_DISPATCHER, 5000, nullptr, subscribe,
      unsubscribe};
  const auto enabled = yukiplt::enable_atfork(config);
  if (!enabled.ok()) {
    errno = enabled.sys_errno != 0 ? enabled.sys_errno : EBUSY;
    return -1;
  }
  end_fork_window();
  const auto callbacks = fork_callbacks;
  callbacks.prepare(callbacks.context);
  yukiplt::ForkStatus status{sizeof(status)};
  const auto observed = yukiplt::fork_status(status);
  if (!observed.ok() || status.reason != YUKIPLT_C_FORK_REASON_NONE ||
      !core_prepare_fork()) {
    callbacks.parent(callbacks.context);
    errno = EBUSY;
    return -1;
  }
  const pid_t result = original();
  const int saved_errno = errno;
  if (result == 0) {
    core_after_fork_child();
    callbacks.child(callbacks.context);
    owner_pid.store(getpid(), std::memory_order_release);
    if (!yukiplt::fork_status(status).ok() ||
        status.phase == YUKIPLT_C_FORK_REJECTED)
      _exit(127);
    // Bionic's atfork callbacks may have changed protections in this child.
    child_cache_ready.store(capture_maps().ok(), std::memory_order_release);
  } else {
    core_after_fork_parent();
    callbacks.parent(callbacks.context);
  }
  errno = saved_errno;
  return result;
}
} // namespace yukizygisk::plt_lifecycle
