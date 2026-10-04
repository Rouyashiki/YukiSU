#pragma once

#include "crash_monitor.hpp"

namespace yukizygisk::history {

struct RuntimeSnapshot {
  std::vector<yz_runtime_record> records;
  uint32_t capabilities = 0;
};

crash::NativeExitContext native_context(const yz_target_exit_event &event,
                                        const RuntimeSnapshot &snapshot);

class Reader {
public:
  using OpenFunction = int (*)(yz_exit_history_fd_cmd *);
  using RuntimeReader = RuntimeSnapshot (*)();
  using ExitHandler = void (*)(const yz_target_exit_event &, uint64_t);

  Reader(uint8_t abi, crash::Monitor &monitor, OpenFunction open)
      : abi_(abi), monitor_(monitor), open_(open) {}
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  Reader(Reader &&) = delete;
  Reader &operator=(Reader &&) = delete;
  void start(bool supported);
  void retry();
  void drain(RuntimeReader runtime, ExitHandler on_exit = nullptr);
  void fail(int error);
  [[nodiscard]] int fd() const { return fd_; }
  [[nodiscard]] bool enabled() const { return enabled_; }
  [[nodiscard]] uint64_t epoch() const {
    return epoch_confirmed_ ? state_.epoch : 0;
  }
  [[nodiscard]] int timeout_ms() const;

private:
  bool validate(const yz_exit_history_header &header,
                const yz_exit_history_record *records, size_t length,
                crash::NativeExitJournalState *next) const;
  uint8_t abi_;
  crash::Monitor &monitor_;
  OpenFunction open_;
  crash::NativeExitJournalState state_;
  int fd_ = -1;
  bool enabled_ = false;
  bool epoch_confirmed_ = false;
  uint64_t retry_at_ns_ = 0;
  unsigned failures_ = 0;
};

} // namespace yukizygisk::history
