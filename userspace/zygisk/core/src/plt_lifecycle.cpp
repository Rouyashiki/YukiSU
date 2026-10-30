#include "plt_lifecycle.hpp"

#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

namespace yukizygisk::plt_lifecycle {
namespace {
pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<pid_t> owner_pid{0};
size_t pending_recoveries = 0;
bool sealed = false;

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
      pthread_mutex_trylock(&operation_mutex) != 0)
    return;
  if ((sealed && access != Access::Retire) ||
      (access == Access::Install && pending_recoveries != 0)) {
    pthread_mutex_unlock(&operation_mutex);
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
    pthread_mutex_unlock(&operation_mutex);
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

pid_t fork_process(pid_t (*original)()) noexcept {
  Operation operation;
  if (!operation || original == nullptr || !single_threaded()) {
    errno = EBUSY;
    return -1;
  }
  const pid_t result = original();
  if (result == 0)
    owner_pid.store(getpid(), std::memory_order_release);
  return result;
}
} // namespace yukizygisk::plt_lifecycle
