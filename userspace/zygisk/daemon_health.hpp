#pragma once

#include <cstddef>
#include <cstdint>

namespace yukizygisk::health {

inline constexpr uint8_t kRequest = 28;
inline constexpr uint32_t kVersion = 1;
inline constexpr int kQueryTimeoutMs = 1000;
inline constexpr char kRecoveryDirectory[] =
    "/data/adb/ksu/yukizygisk/recovery";
inline constexpr char kEnsureLock[] =
    "/data/adb/ksu/yukizygisk/recovery/ensure.lock";

inline const char *lock_path(uint32_t abi) {
  return abi == 1 ? "/data/adb/ksu/yukizygisk/recovery/daemon32.lock"
                  : "/data/adb/ksu/yukizygisk/recovery/daemon64.lock";
}

inline const char *catalog_name(uint32_t abi) {
  return abi == 1 ? "catalog32.json" : "catalog64.json";
}

enum class ReaderState : uint32_t { Unknown, Unsupported, Active, Retrying };
enum Flags : uint32_t {
  Ready = 1U << 0,
  HistoryAvailable = 1U << 1,
  PersistencePending = 1U << 2,
  CommittedValid = 1U << 3,
  CatalogFrozen = 1U << 4,
  RebootRequired = 1U << 5,
};

struct alignas(8) Snapshot {
  uint32_t version = kVersion;
  uint32_t size = sizeof(Snapshot);
  uint32_t abi = 0;
  uint32_t pid = 0;
  uint64_t start_ticks = 0;
  uint64_t sampled_boottime_ns = 0;
  uint64_t consumed_epoch = 0;
  uint64_t consumed_cursor = 0;
  uint64_t newest_observed = 0;
  uint64_t committed_epoch = 0;
  uint64_t committed_cursor = 0;
  uint64_t read_retry_at_ns = 0;
  uint64_t read_failures = 0;
  uint64_t save_retry_at_ns = 0;
  uint64_t save_failures = 0;
  uint64_t last_saved_boottime_ns = 0;
  uint64_t last_read_boottime_ns = 0;
  ReaderState reader_state = ReaderState::Unknown;
  int32_t read_error = 0;
  int32_t save_error = 0;
  uint32_t flags = 0;
  uint32_t bound_sessions = 0;
  uint32_t unbound_sessions = 0;
  uint32_t companions = 0;
  uint32_t starting_companions = 0;
  uint32_t terminating_companions = 0;
  int32_t catalog_error = 0;
  char catalog_sha256[64]{};
  uint64_t poll_returns = 0;
  uint64_t poll_timeouts = 0;
  uint64_t history_drains = 0;
  uint64_t reserved64 = 0;
};

static_assert(sizeof(Snapshot) == 256);
static_assert(alignof(Snapshot) == 8);
static_assert(offsetof(Snapshot, start_ticks) == 16);
static_assert(offsetof(Snapshot, reader_state) == 120);
static_assert(offsetof(Snapshot, catalog_sha256) == 160);

inline constexpr char kReady = '1';
inline constexpr char kFailed = '0';
inline constexpr char kRebootRequired = 'R';

} // namespace yukizygisk::health
