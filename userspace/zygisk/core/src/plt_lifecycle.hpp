#pragma once

#include <sys/types.h>
#include <yukiplt.hpp>

namespace yukizygisk::plt_lifecycle {

enum class Access { Install, Recover, Retire, Fork };

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

// Acquire before loader locks or external callbacks. Same-thread nesting and
// Operation calls are allowed; unrelated mapping writers must cooperate too.
class ExternalScope {
public:
  explicit ExternalScope(unsigned timeout_ms = 5000) noexcept;
  ~ExternalScope();
  ExternalScope(const ExternalScope &) = delete;
  ExternalScope &operator=(const ExternalScope &) = delete;
  ExternalScope(ExternalScope &&) = delete;
  ExternalScope &operator=(ExternalScope &&) = delete;
  explicit operator bool() const noexcept { return error_ == 0; }
  int error() const noexcept { return error_; }

private:
  bool owner_ = false;
  int error_ = 0;
};

// Failure stops dispatch, ordinary operations and retirement. Managed fork
// remains available once outstanding operations have completed.
bool dispatch_allowed() noexcept;

// Called only while holding Operation; each owner keeps its own flag.
void set_recovery(bool &owner_pending, bool pending) noexcept;
bool recovery_pending() noexcept;
bool retirement_window_ready(bool private_tsd_live) noexcept;
// Cache and dispatcher access, including invalidation, requires Operation.
yukiplt::MapsCache &maps_cache() noexcept;
yukiplt::Error capture_maps() noexcept;
void invalidate_fork_cache() noexcept;
yukiplt::Error unhook(yukiplt::HookHandle &handle, yukiplt::SlotReport *slots,
                      size_t count, yukiplt::OperationDiagnostic *diagnostic);
bool shutdown() noexcept;
// Parent initialization and private loader work must finish before this call.
// These managed entries rely on bionic fork to coordinate the system runtime.
pid_t fork_process(pid_t (*original)()) noexcept;

} // namespace yukizygisk::plt_lifecycle
