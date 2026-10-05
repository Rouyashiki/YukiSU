#pragma once

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>

namespace yuki::ihook {

enum class HookPhase : uint8_t {
  Prepared,
  PublishedPendingSync,
  Installed,
  RestoredPendingSync,
  RestoredPendingReclaim,
  Released,
  Indeterminate,
};

enum class SyncOperation : uint8_t { Prepare, Publish, Restore };
enum class SyncResult : uint8_t { Confirmed, Indeterminate };

namespace lifecycle_detail {

#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
#if defined(__arm__)
[[nodiscard]] __attribute__((target("arm"), noinline, no_stack_protector))
#else
[[nodiscard]] __attribute__((noinline, no_stack_protector))
#endif
inline long raw_call(long number, uintptr_t first = 0, uintptr_t second = 0,
                     uintptr_t third = 0, uintptr_t fourth = 0) {
#if defined(__arm__)
  register long r0 asm("r0") = static_cast<long>(first);
  register long r1 asm("r1") = static_cast<long>(second);
  register long r2 asm("r2") = static_cast<long>(third);
  register long r3 asm("r3") = static_cast<long>(fourth);
  register long r7 asm("r7") = number;
  asm volatile("svc #0"
               : "+r"(r0)
               : "r"(r1), "r"(r2), "r"(r3), "r"(r7)
               : "memory", "cc");
  return r0;
#elif defined(__aarch64__)
  register long x0 asm("x0") = static_cast<long>(first);
  register long x1 asm("x1") = static_cast<long>(second);
  register long x2 asm("x2") = static_cast<long>(third);
  register long x3 asm("x3") = static_cast<long>(fourth);
  register long x8 asm("x8") = number;
  asm volatile("svc #0"
               : "+r"(x0)
               : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
               : "memory", "cc");
  return x0;
#else
  register uintptr_t r10 asm("r10") = fourth;
  long result;
  asm volatile("syscall"
               : "=a"(result)
               : "a"(number), "D"(first), "S"(second), "d"(third), "r"(r10)
               : "rcx", "r11", "memory", "cc");
  return result;
#endif
}
#endif

} // namespace lifecycle_detail

[[noreturn]] inline void fail_indeterminate(HookPhase *phase) {
  if (phase != nullptr)
    *phase = HookPhase::Indeterminate;
  static constexpr char message[] =
      "yuki_ihook: patch execution state indeterminate\n";
#if defined(__linux__) &&                                                      \
    (defined(__arm__) || defined(__aarch64__) || defined(__x86_64__))
  using lifecycle_detail::raw_call;
  (void)raw_call(__NR_write, STDERR_FILENO,
                 reinterpret_cast<uintptr_t>(message), sizeof(message) - 1);
  // Every supported kernel action layout represents SIG_DFL with zero fields.
  volatile unsigned long action[8];
  for (auto &word : action)
    word = 0;
  const uint64_t mask = uint64_t{1} << (SIGABRT - 1);
  const long disposition =
      raw_call(__NR_rt_sigaction, SIGABRT, reinterpret_cast<uintptr_t>(action),
               0, sizeof(mask));
  const long unblocked =
      raw_call(__NR_rt_sigprocmask, SIG_UNBLOCK,
               reinterpret_cast<uintptr_t>(&mask), 0, sizeof(mask));
  if (disposition == 0 && unblocked == 0) {
    const long pid = raw_call(__NR_getpid);
    const long tid = raw_call(__NR_gettid);
    if (pid > 0 && tid > 0)
      (void)raw_call(__NR_tgkill, static_cast<uintptr_t>(pid),
                     static_cast<uintptr_t>(tid), SIGABRT);
  }
  (void)raw_call(__NR_exit_group, 128 + SIGABRT);
  __builtin_trap();
#else
  const ssize_t ignored = write(STDERR_FILENO, message, sizeof(message) - 1);
  (void)ignored;
  abort();
#endif
}

} // namespace yuki::ihook
