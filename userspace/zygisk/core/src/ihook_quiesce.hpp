#pragma once

#include "ihook_lifecycle.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <type_traits>
#include <ucontext.h>
#include <unistd.h>

#if defined(__linux__)
#ifndef __NR_rt_tgsigqueueinfo
#if defined(__aarch64__)
#define __NR_rt_tgsigqueueinfo 240
#elif defined(__arm__)
#define __NR_rt_tgsigqueueinfo 363
#elif defined(__x86_64__)
#define __NR_rt_tgsigqueueinfo 297
#endif
#endif // #ifndef __NR_rt_tgsigqueueinfo
#endif

namespace yuki::ihook {

namespace quiesce_detail {

inline constexpr size_t kMaxPeerThreads = 256;
inline constexpr size_t kTaskBufferSize = 4096;

struct KernelDirent64 {
  uint64_t inode;
  int64_t offset;
  uint16_t record_size;
  uint8_t type;
  char name[1];
};

struct QuiesceState {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  std::atomic<int> phase{0};
  std::atomic<uint32_t> active_epoch{0};
  std::atomic<uint32_t> entered{0};
  std::atomic<uint32_t> exited{0};
  std::atomic<uint32_t> conflict{0};
  std::atomic<uint32_t> in_handler{0};
  std::atomic<uintptr_t> guard_start{0};
  std::atomic<uintptr_t> guard_end{0};
  std::atomic<bool> use_sigqueue{false};
  int installed_sig = -1;
  struct sigaction old_action{};
};

static_assert(std::is_trivially_destructible_v<QuiesceState>,
              "QuiesceState must remain trivially destructible");
static_assert(std::atomic<int>::is_always_lock_free,
              "quiesce phase must be lock-free");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "quiesce counters must be lock-free");
static_assert(std::atomic<uintptr_t>::is_always_lock_free,
              "quiesce guard range must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free,
              "quiesce sigqueue flag must be lock-free");

inline QuiesceState &state() {
  static QuiesceState instance;
  return instance;
}

[[nodiscard]] inline uintptr_t context_pc(const void *uctx) {
  if (uctx == nullptr)
    return 0;
#if defined(__aarch64__)
  return static_cast<uintptr_t>(
      static_cast<const ucontext_t *>(uctx)->uc_mcontext.pc);
#elif defined(__arm__)
  return static_cast<uintptr_t>(
             static_cast<const ucontext_t *>(uctx)->uc_mcontext.arm_pc) &
         ~uintptr_t{1};
#else
  (void)uctx;
  return 0;
#endif
}

inline void quiesce_handler(int, siginfo_t *info, void *uctx) {
  QuiesceState &st = state();
  st.in_handler.fetch_add(1U, std::memory_order_acq_rel);
  if (st.phase.load(std::memory_order_acquire) == 1) {
    const bool check_epoch = st.use_sigqueue.load(std::memory_order_acquire) &&
                             info != nullptr && info->si_code == SI_QUEUE;
    if (!check_epoch || static_cast<uint32_t>(info->si_int) ==
                            st.active_epoch.load(std::memory_order_acquire)) {
      const uintptr_t g_start = st.guard_start.load(std::memory_order_relaxed);
      const uintptr_t g_end = st.guard_end.load(std::memory_order_relaxed);
      const uintptr_t pc = context_pc(uctx);
      if (pc != 0 && g_start < g_end && pc >= g_start && pc < g_end) {
        st.conflict.fetch_add(1U, std::memory_order_relaxed);
      }
      st.entered.fetch_add(1U, std::memory_order_release);
      while (st.phase.load(std::memory_order_acquire) == 1) {
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
        (void)lifecycle_detail::raw_call(__NR_sched_yield);
#endif
      }
      st.exited.fetch_add(1U, std::memory_order_release);
    }
  }
  st.in_handler.fetch_sub(1U, std::memory_order_release);
}

[[nodiscard]] inline bool parse_tid(const char *name, pid_t *out) {
  if (name == nullptr || out == nullptr || name[0] < '0' || name[0] > '9')
    return false;
  unsigned long val = 0;
  for (const char *p = name; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9')
      return false;
    val = (val * 10UL) + static_cast<unsigned long>(*p - '0');
    if (val > 0x7fffffffUL)
      return false;
  }
  if (val == 0)
    return false;
  *out = static_cast<pid_t>(val);
  return true;
}

[[nodiscard]] inline uint64_t monotonic_ms() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (static_cast<uint64_t>(ts.tv_sec) * 1000ULL) +
         (static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL);
}

[[nodiscard]] inline int find_unused_rt_signal() {
  const int lo = SIGRTMIN + 4;
  const int hi = SIGRTMAX;
  if (lo > hi)
    return -1;
  for (int sig = hi; sig >= lo; --sig) {
    struct sigaction cur{};
    if (sigaction(sig, nullptr, &cur) != 0)
      continue;
    if ((cur.sa_flags & SA_SIGINFO) == 0 && cur.sa_handler == SIG_DFL)
      return sig;
  }
  return -1;
}

[[nodiscard]] inline long send_tgkill(pid_t pid, pid_t tid, int sig) {
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  return lifecycle_detail::raw_call(__NR_tgkill, static_cast<uintptr_t>(pid),
                                    static_cast<uintptr_t>(tid),
                                    static_cast<uintptr_t>(sig));
#else
  (void)pid;
  (void)tid;
  (void)sig;
  return -ENOSYS;
#endif
}

[[nodiscard]] inline long send_quiesce_signal(pid_t pid, pid_t tid, int sig,
                                              uint32_t epoch, uid_t uid,
                                              bool *used_sigqueue) {
#if defined(__linux__) && defined(__NR_rt_tgsigqueueinfo) &&                   \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  siginfo_t info{};
  info.si_signo = sig;
  info.si_code = SI_QUEUE;
  info.si_pid = pid;
  info.si_uid = uid;
  info.si_int = static_cast<int>(epoch);
  const long rc = lifecycle_detail::raw_call(
      __NR_rt_tgsigqueueinfo, static_cast<uintptr_t>(pid),
      static_cast<uintptr_t>(tid), static_cast<uintptr_t>(sig),
      reinterpret_cast<uintptr_t>(&info));
  if (rc == 0) {
    *used_sigqueue = true;
    return 0;
  }
  if (rc == -ESRCH)
    return -ESRCH;
#else
  (void)epoch;
  (void)uid;
#endif
  *used_sigqueue = false;
  return send_tgkill(pid, tid, sig);
}

[[nodiscard]] inline bool tid_alive(pid_t pid, pid_t tid) {
  const long rc = send_tgkill(pid, tid, 0);
  return rc != -ESRCH;
}

[[nodiscard]] inline bool list_peer_tids(pid_t self_tid, pid_t *out,
                                         size_t capacity, size_t *count_out,
                                         uint8_t (&buffer)[kTaskBufferSize]) {
  *count_out = 0;
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  using lifecycle_detail::raw_call;
  const long fd = raw_call(__NR_openat, static_cast<uintptr_t>(AT_FDCWD),
                           reinterpret_cast<uintptr_t>("/proc/self/task"),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return false;

  constexpr size_t name_offset = offsetof(KernelDirent64, name);
  size_t count = 0;
  bool complete = true;
  while (complete) {
    const long read =
        raw_call(__NR_getdents64, static_cast<uintptr_t>(fd),
                 reinterpret_cast<uintptr_t>(buffer), sizeof(buffer));
    if (read == -EINTR)
      continue;
    if (read <= 0 || static_cast<size_t>(read) > sizeof(buffer)) {
      complete = read == 0;
      break;
    }
    const size_t bytes = static_cast<size_t>(read);
    for (size_t offset = 0; offset < bytes;) {
      if (bytes - offset <= name_offset) {
        complete = false;
        break;
      }
      uint16_t record_size = 0;
      memcpy(&record_size,
             buffer + offset + offsetof(KernelDirent64, record_size),
             sizeof(record_size));
      if (record_size <= name_offset || record_size > bytes - offset) {
        complete = false;
        break;
      }
      const auto *name =
          reinterpret_cast<const char *>(buffer + offset + name_offset);
      if (memchr(name, '\0', record_size - name_offset) == nullptr) {
        complete = false;
        break;
      }
      offset += record_size;
      pid_t tid = 0;
      if (!parse_tid(name, &tid) || tid == self_tid)
        continue;
      if (count >= capacity) {
        complete = false;
        break;
      }
      out[count++] = tid;
    }
  }
  const long closed = raw_call(__NR_close, static_cast<uintptr_t>(fd));
  *count_out = count;
  return complete && closed == 0;
#else
  (void)self_tid;
  (void)out;
  (void)capacity;
  (void)buffer;
  return false;
#endif
}

} // namespace quiesce_detail

class QuiesceGuard {
public:
  QuiesceGuard() = default;
  QuiesceGuard(const QuiesceGuard &) = delete;
  QuiesceGuard &operator=(const QuiesceGuard &) = delete;
  QuiesceGuard(QuiesceGuard &&) = delete;
  QuiesceGuard &operator=(QuiesceGuard &&) = delete;
  ~QuiesceGuard() { release(); }

  [[nodiscard]] bool acquire(uintptr_t guard_start = 0,
                             uintptr_t guard_end = 0) {
    if (acquired_)
      return false;
    quiesce_detail::QuiesceState &st = quiesce_detail::state();
    pthread_mutex_lock(&st.mutex);
    locked_ = true;

    const pid_t pid = getpid();
    const uid_t uid = getuid();
    const pid_t self_tid = static_cast<pid_t>(
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
        lifecycle_detail::raw_call(__NR_gettid)
#else
        syscall(SYS_gettid)
#endif
    );

    alignas(uint64_t) uint8_t task_buffer[quiesce_detail::kTaskBufferSize];
    pid_t initial_peers[quiesce_detail::kMaxPeerThreads]{};
    size_t initial_count = 0;
    if (!quiesce_detail::list_peer_tids(self_tid, initial_peers,
                                        quiesce_detail::kMaxPeerThreads,
                                        &initial_count, task_buffer)) {
      release();
      return false;
    }
    if (initial_count == 0) {
      if (st.installed_sig >= 0 &&
          st.in_handler.load(std::memory_order_acquire) == 0) {
        (void)sigaction(st.installed_sig, &st.old_action, nullptr);
        st.installed_sig = -1;
      }
      acquired_ = true;
      paused_ = false;
      all_entered_ = true;
      return true;
    }

    int sig = st.installed_sig;
    if (sig < 0) {
      sig = quiesce_detail::find_unused_rt_signal();
      if (sig < 0) {
        release();
        return false;
      }
      struct sigaction sa{};
      sa.sa_sigaction = quiesce_detail::quiesce_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
      if (sigaction(sig, &sa, &st.old_action) != 0) {
        release();
        return false;
      }
      st.installed_sig = sig;
    }
    sig_ = sig;

    constexpr unsigned kMaxAttempts = 5;
    for (unsigned attempt = 0; attempt != kMaxAttempts; ++attempt) {
      uint32_t epoch = st.active_epoch.load(std::memory_order_relaxed) + 1U;
      if (epoch == 0)
        epoch = 1U;
      st.active_epoch.store(epoch, std::memory_order_release);
      st.guard_start.store(guard_start, std::memory_order_relaxed);
      st.guard_end.store(guard_end, std::memory_order_relaxed);
      st.use_sigqueue.store(true, std::memory_order_release);
      st.conflict.store(0U, std::memory_order_relaxed);
      st.entered.store(0U, std::memory_order_relaxed);
      st.exited.store(0U, std::memory_order_relaxed);
      st.phase.store(1, std::memory_order_release);

      pid_t sent_tids[quiesce_detail::kMaxPeerThreads]{};
      bool sent_alive[quiesce_detail::kMaxPeerThreads]{};
      size_t sent_count = 0;
      uint32_t expected = 0;
      bool dispatch_ok = true;
      bool peers_stable = false;

      for (unsigned round = 0; round != 3U; ++round) {
        pid_t peers[quiesce_detail::kMaxPeerThreads]{};
        size_t peer_count = 0;
        if (!quiesce_detail::list_peer_tids(self_tid, peers,
                                            quiesce_detail::kMaxPeerThreads,
                                            &peer_count, task_buffer)) {
          dispatch_ok = false;
          break;
        }
        bool added_any = false;
        for (size_t i = 0; i != peer_count; ++i) {
          const pid_t tid = peers[i];
          bool seen = false;
          for (size_t j = 0; j != sent_count; ++j) {
            if (sent_tids[j] == tid) {
              seen = true;
              break;
            }
          }
          if (seen)
            continue;
          if (sent_count >= quiesce_detail::kMaxPeerThreads) {
            dispatch_ok = false;
            break;
          }
          bool used_sq = true;
          const long rc = quiesce_detail::send_quiesce_signal(
              pid, tid, sig_, epoch, uid, &used_sq);
          if (!used_sq)
            st.use_sigqueue.store(false, std::memory_order_release);
          if (rc == 0) {
            sent_tids[sent_count] = tid;
            sent_alive[sent_count] = true;
            ++sent_count;
            ++expected;
            added_any = true;
          } else if (rc != -ESRCH) {
            dispatch_ok = false;
            break;
          }
        }
        if (!dispatch_ok || !added_any) {
          peers_stable = dispatch_ok && !added_any;
          break;
        }
        const uint64_t wait_deadline = quiesce_detail::monotonic_ms() + 50ULL;
        while (st.entered.load(std::memory_order_acquire) < expected) {
          for (size_t j = 0; j != sent_count; ++j) {
            if (sent_alive[j] &&
                !quiesce_detail::tid_alive(pid, sent_tids[j])) {
              sent_alive[j] = false;
              if (expected > 0)
                --expected;
            }
          }
          if (st.entered.load(std::memory_order_acquire) >= expected)
            break;
          if (quiesce_detail::monotonic_ms() >= wait_deadline) {
            dispatch_ok = false;
            break;
          }
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
          (void)lifecycle_detail::raw_call(__NR_sched_yield);
#endif
        }
        if (!dispatch_ok)
          break;
      }

      dispatch_ok = dispatch_ok && peers_stable;
      if (dispatch_ok) {
        const uint64_t settle_deadline = quiesce_detail::monotonic_ms() + 50ULL;
        while (st.entered.load(std::memory_order_acquire) < expected) {
          for (size_t j = 0; j != sent_count; ++j) {
            if (sent_alive[j] &&
                !quiesce_detail::tid_alive(pid, sent_tids[j])) {
              sent_alive[j] = false;
              if (expected > 0)
                --expected;
            }
          }
          if (st.entered.load(std::memory_order_acquire) >= expected)
            break;
          if (quiesce_detail::monotonic_ms() >= settle_deadline) {
            dispatch_ok = false;
            break;
          }
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
          (void)lifecycle_detail::raw_call(__NR_sched_yield);
#endif
        }
      }

      if (!dispatch_ok) {
        paused_ = true;
        all_entered_ = false;
        release();
        return false;
      }

      if (st.conflict.load(std::memory_order_acquire) == 0) {
        acquired_ = true;
        paused_ = true;
        all_entered_ = true;
        return true;
      }

      resume_peers(true, false);
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
      (void)lifecycle_detail::raw_call(__NR_sched_yield);
#endif
    }

    all_entered_ = true;
    paused_ = false;
    release();
    return false;
  }

  void release() {
    if (paused_) {
      resume_peers(all_entered_, true);
      paused_ = false;
    } else if (sig_ >= 0) {
      resume_peers(all_entered_, true);
    }
    acquired_ = false;
    all_entered_ = false;
    sig_ = -1;
    if (locked_) {
      locked_ = false;
      pthread_mutex_unlock(&quiesce_detail::state().mutex);
    }
  }

private:
  static void resume_peers(bool all_entered, bool uninstall_handler) {
    quiesce_detail::QuiesceState &st = quiesce_detail::state();
    st.phase.store(2, std::memory_order_release);
    const uint64_t deadline = quiesce_detail::monotonic_ms() + 50ULL;
    while (st.exited.load(std::memory_order_acquire) <
               st.entered.load(std::memory_order_acquire) ||
           st.in_handler.load(std::memory_order_acquire) != 0) {
      if (quiesce_detail::monotonic_ms() >= deadline)
        break;
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
      (void)lifecycle_detail::raw_call(__NR_sched_yield);
#endif
    }
    const bool settled = st.exited.load(std::memory_order_acquire) >=
                             st.entered.load(std::memory_order_acquire) &&
                         st.in_handler.load(std::memory_order_acquire) == 0;
    st.phase.store(0, std::memory_order_release);
    if (uninstall_handler && all_entered && settled && st.installed_sig >= 0) {
      (void)sigaction(st.installed_sig, &st.old_action, nullptr);
      st.installed_sig = -1;
    }
  }

  bool locked_ = false;
  bool acquired_ = false;
  bool paused_ = false;
  bool all_entered_ = false;
  int sig_ = -1;
};

[[nodiscard]] inline bool quiesce_idle() {
  quiesce_detail::QuiesceState &st = quiesce_detail::state();
  pthread_mutex_lock(&st.mutex);
  const bool idle = st.installed_sig < 0 &&
                    st.in_handler.load(std::memory_order_acquire) == 0;
  pthread_mutex_unlock(&st.mutex);
  return idle;
}

} // namespace yuki::ihook
