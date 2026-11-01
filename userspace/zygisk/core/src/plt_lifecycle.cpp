#include "plt_lifecycle.hpp"
#include "core_bindings.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <ctime>
#include <dirent.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace yukizygisk::plt_lifecycle {
namespace {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(pid_t) == sizeof(uint32_t));
constexpr uint64_t kThreadMask = (uint64_t{1} << 31) - 1;
constexpr uint64_t kExternalMask = kThreadMask << 31;
constexpr uint64_t kDraining = uint64_t{1} << 62;
constexpr uint64_t kDispatchFailed = uint64_t{1} << 63;
// Publish both owners together so a boundary cannot miss an operation.
std::atomic<uint64_t> admission{0};
std::atomic<uint32_t> operation_epoch{0};
std::atomic<pid_t> owner_pid{0};
size_t pending_recoveries = 0;
bool sealed = false;
yukiplt::MapsCache cache;
constexpr uint64_t kCacheExhausted = UINT64_MAX - 1;
std::atomic<uint64_t> cache_window{0};
yukiplt_c_fork_callbacks fork_callbacks{};
uint64_t subscription = 0;
uint64_t next_subscription = 1;

uint64_t external_thread(uint64_t state) noexcept {
  return (state & kExternalMask) >> 31;
}

bool own_process() noexcept {
  const pid_t current = getpid();
  pid_t empty = 0;
  owner_pid.compare_exchange_strong(empty, current, std::memory_order_acq_rel);
  return owner_pid.load(std::memory_order_acquire) == current;
}

void release_operation() noexcept {
  if (admission.fetch_and(~kThreadMask, std::memory_order_release) &
      kDraining) {
    const int saved = errno;
    operation_epoch.fetch_add(1, std::memory_order_release);
    syscall(SYS_futex, &operation_epoch, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr,
            nullptr, 0);
    errno = saved;
  }
}

uint64_t close_cache_window() noexcept {
  uint64_t state = cache_window.load(std::memory_order_acquire);
  for (;;) {
    const uint64_t next = state >= kCacheExhausted - 2
                              ? kCacheExhausted
                              : (state & ~uint64_t{1}) + 2;
    if (cache_window.compare_exchange_weak(
            state, next, std::memory_order_acq_rel, std::memory_order_acquire))
      return next;
  }
}

int monotonic_ns(uint64_t &value) noexcept {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return errno;
  value = static_cast<uint64_t>(now.tv_sec) * 1000000000 +
          static_cast<uint64_t>(now.tv_nsec);
  return 0;
}

int drain_operation(unsigned timeout_ms) noexcept {
  uint64_t now = 0;
  int error = monotonic_ns(now);
  if (error != 0)
    return error;
  const uint64_t deadline = now + (uint64_t{timeout_ms} * 1000000);
  for (;;) {
    const auto epoch = operation_epoch.load(std::memory_order_acquire);
    if ((admission.load(std::memory_order_acquire) & kThreadMask) == 0)
      return 0;
    error = monotonic_ns(now);
    if (error != 0)
      return error;
    if (now >= deadline)
      return ETIMEDOUT;
    const uint64_t remaining = deadline - now;
    const timespec timeout{static_cast<time_t>(remaining / 1000000000),
                           static_cast<long>(remaining % 1000000000)};
    if (syscall(SYS_futex, &operation_epoch, FUTEX_WAIT_PRIVATE, epoch,
                &timeout, nullptr, 0) == -1 &&
        errno != EINTR && errno != EAGAIN && errno != ETIMEDOUT)
      return errno;
  }
}

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
  if (!own_process())
    return;
  const auto thread = static_cast<uint64_t>(gettid());
  uint64_t state = admission.load(std::memory_order_acquire);
  for (;;) {
    if ((state & (kThreadMask | kDraining)) != 0 ||
        ((state & kDispatchFailed) != 0 && access != Access::Fork) ||
        (external_thread(state) != 0 && external_thread(state) != thread))
      return;
    if (admission.compare_exchange_weak(state, state | thread,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire))
      break;
  }
  if ((sealed && access != Access::Retire) ||
      ((access == Access::Install || access == Access::Fork) &&
       pending_recoveries != 0)) {
    release_operation();
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
    release_operation();
}

ExternalScope::ExternalScope(unsigned timeout_ms) noexcept {
  const int saved = errno;
  if (!own_process()) {
    error_ = ECHILD;
    return;
  }
  const auto thread = static_cast<uint64_t>(gettid());
  uint64_t state = admission.load(std::memory_order_acquire);
  for (;;) {
    if (state & kDispatchFailed) {
      error_ = ECANCELED;
      break;
    }
    if ((state & kThreadMask) == thread) {
      error_ = EDEADLK;
      break;
    }
    if (external_thread(state) != 0) {
      if (external_thread(state) != thread || (state & kDraining))
        error_ = EBUSY;
      break;
    }
    if (admission.compare_exchange_weak(
            state, state | (thread << 31) | kDraining,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      owner_ = true;
      break;
    }
  }
  close_cache_window();
  if (owner_) {
    error_ = drain_operation(timeout_ms);
    if (error_ == 0) {
      const uint64_t previous =
          admission.fetch_and(~kDraining, std::memory_order_acq_rel);
      if ((previous & kDispatchFailed) != 0 || sealed)
        error_ = ECANCELED;
    }
  }
  if (error_ != 0) {
    admission.fetch_or(kDispatchFailed, std::memory_order_acq_rel);
    if (owner_) {
      admission.fetch_and(~(kExternalMask | kDraining),
                          std::memory_order_release);
      owner_ = false;
    }
  }
  errno = saved;
}

ExternalScope::~ExternalScope() {
  if (owner_)
    admission.fetch_and(~kExternalMask, std::memory_order_release);
}

bool dispatch_allowed() noexcept {
  return (admission.load(std::memory_order_acquire) & kDispatchFailed) == 0;
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
  return dispatch_allowed() && getpid() == gettid() &&
         owner_pid.load(std::memory_order_acquire) == getpid() &&
         (!private_tsd_live || single_threaded());
}

yukiplt::MapsCache &maps_cache() noexcept { return cache; }

yukiplt::Error capture_maps() noexcept {
  invalidate_fork_cache();
  return cache.capture();
}

void invalidate_fork_cache() noexcept { close_cache_window(); }

yukiplt::Error unhook(yukiplt::HookHandle &handle, yukiplt::SlotReport *slots,
                      size_t count, yukiplt::OperationDiagnostic *diagnostic) {
  if ((cache_window.load(std::memory_order_acquire) & 1) == 0)
    return yukiplt::unhook_diagnosed(handle, slots, count, diagnostic);
  const auto error =
      yukiplt::unhook_cached(cache, handle, slots, count, diagnostic);
  if (!error.ok())
    invalidate_fork_cache();
  return error;
}

bool shutdown() noexcept {
  if (!dispatch_allowed() || cache.has_bindings() ||
      !yukiplt::disable_atfork().ok())
    return false;
  invalidate_fork_cache();
  cache = {};
  return true;
}

pid_t fork_process(pid_t (*original)()) noexcept {
  Operation operation(Access::Fork);
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
  invalidate_fork_cache();
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
  const auto fork_thread = static_cast<uint64_t>(gettid());
  const pid_t result = original();
  const int saved_errno = errno;
  if (result == 0) {
    core_after_fork_child();
    callbacks.child(callbacks.context);
    const uint64_t inherited = admission.load(std::memory_order_acquire);
    const auto child_thread = static_cast<uint64_t>(gettid());
    uint64_t child_state = child_thread | (inherited & kDispatchFailed);
    if (external_thread(inherited) == fork_thread && !(inherited & kDraining))
      child_state |= child_thread << 31;
    admission.store(child_state, std::memory_order_release);
    operation_epoch.store(0, std::memory_order_release);
    owner_pid.store(getpid(), std::memory_order_release);
    if (!yukiplt::fork_status(status).ok() ||
        status.phase == YUKIPLT_C_FORK_REJECTED)
      _exit(127);
    // Bionic's atfork callbacks may have changed protections in this child.
    uint64_t window = close_cache_window();
    if (cache.capture().ok() && window != kCacheExhausted &&
        (admission.load(std::memory_order_acquire) &
         (kExternalMask | kDraining | kDispatchFailed)) == 0)
      cache_window.compare_exchange_strong(window, window | 1,
                                           std::memory_order_acq_rel);
  } else {
    core_after_fork_parent();
    callbacks.parent(callbacks.context);
  }
  errno = saved_errno;
  return result;
}
} // namespace yukizygisk::plt_lifecycle
