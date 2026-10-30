#pragma once

#include <sys/types.h>

namespace yukizygisk::plt_lifecycle {

enum class Access { Install, Recover, Retire };

class Operation {
public:
  explicit Operation(Access access = Access::Install) noexcept;
  ~Operation();
  void seal() const noexcept;
  Operation(const Operation &) = delete;
  Operation &operator=(const Operation &) = delete;
  explicit operator bool() const noexcept { return acquired_; }

private:
  bool acquired_ = false;
};

// Called only while holding Operation; each owner keeps its own flag.
void set_recovery(bool &owner_pending, bool pending) noexcept;
bool recovery_pending() noexcept;
bool retirement_window_ready(bool private_tsd_live) noexcept;
pid_t fork_process(pid_t (*original)()) noexcept;

} // namespace yukizygisk::plt_lifecycle
