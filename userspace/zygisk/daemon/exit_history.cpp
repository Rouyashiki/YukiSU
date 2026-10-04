#include "exit_history.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <limits>

namespace yukizygisk::history {
namespace {
uint64_t now_ns() {
  timespec now{};
  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0)
    return 0;
  return (static_cast<uint64_t>(now.tv_sec) * 1000000000) +
         static_cast<uint64_t>(now.tv_nsec);
}

uint64_t add_saturated(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

uint8_t saved_state(const yz_runtime_record &record, uint32_t capabilities) {
  if ((capabilities & YZ_RUNTIME_CAP_INJECTION_STATE) == 0)
    return 0;
  const auto state =
      static_cast<uint8_t>((record.flags & YZ_RUNTIME_F_INJECTION_STATE_MASK) >>
                           YZ_RUNTIME_F_INJECTION_STATE_SHIFT);
  return state >= YZ_RUNTIME_STATE_DETECTED &&
                 state <= YZ_RUNTIME_STATE_SAFEMODE
             ? state
             : 0;
}
} // namespace

crash::NativeExitContext native_context(const yz_target_exit_event &event,
                                        const RuntimeSnapshot &snapshot) {
  crash::NativeExitContext context;
  const yz_runtime_record *base = nullptr;
  for (const auto &record : snapshot.records) {
    if (record.pid == event.event.pid &&
        record.generation == event.generation &&
        record.kind == YZ_RUNTIME_KIND_NATIVE && record.abi == event.abi &&
        record.module_id[0] == '\0' &&
        record.state == YZ_RUNTIME_STATE_EXITED) {
      base = &record;
      break;
    }
  }
  if (base == nullptr)
    return context;
  context.process.assign(base->process,
                         strnlen(base->process, sizeof(base->process)));
  context.target.assign(base->target,
                        strnlen(base->target, sizeof(base->target)));
  context.target_type = base->target_type;
  context.state = saved_state(*base, snapshot.capabilities);
  for (const auto &record : snapshot.records) {
    if (record.pid != event.event.pid ||
        record.generation != event.generation ||
        record.kind != YZ_RUNTIME_KIND_NATIVE || record.abi != event.abi ||
        record.module_id[0] == '\0' ||
        record.state != YZ_RUNTIME_STATE_EXITED ||
        record.target_type != base->target_type ||
        strncmp(record.target, base->target, sizeof(record.target)) != 0)
      continue;
    context.modules.push_back(
        {std::string(record.module_id,
                     strnlen(record.module_id, sizeof(record.module_id))),
         saved_state(record, snapshot.capabilities)});
  }
  context.modules_observed = !context.modules.empty();
  return context;
}

Reader::~Reader() {
  if (fd_ >= 0)
    close(fd_);
}

void Reader::start(bool supported) {
  if (fd_ >= 0)
    close(fd_);
  fd_ = -1;
  enabled_ = supported;
  epoch_confirmed_ = false;
  state_ = monitor_.native_journal();
  state_.available = supported;
  failures_ = 0;
  retry_at_ns_ = 0;
  if (supported) {
    retry();
  } else {
    state_.reader_state = "unsupported";
    state_.last_error = 0;
    state_.observer_active = false;
    state_.observed_boottime_ns = now_ns();
    monitor_.set_native_journal(state_);
    monitor_.flush_native();
  }
}

void Reader::retry() {
  if (!enabled_ || fd_ >= 0 || now_ns() < retry_at_ns_)
    return;
  yz_exit_history_fd_cmd command{};
  command.epoch = state_.epoch;
  command.after_sequence = state_.cursor;
  command.fd = -1;
  if (open_(&command) != 0 || command.fd < 0) {
    const int error = errno ? errno : EIO;
    if (error == ENOTTY || error == EOPNOTSUPP) {
      start(false);
      return;
    }
    if (error == EINVAL && state_.cursor != 0) {
      state_ = {};
      state_.available = true;
    }
    fail(error);
    return;
  }
  fd_ = command.fd;
  retry_at_ns_ = 0;
}

void Reader::fail(int error) {
  if (!enabled_)
    return;
  if (fd_ >= 0)
    close(fd_);
  fd_ = -1;
  epoch_confirmed_ = false;
  constexpr std::array<unsigned, 5> delays{1, 2, 5, 10, 30};
  const auto index = std::min<size_t>(failures_, delays.size() - 1);
  if (failures_ < delays.size())
    ++failures_;
  const uint64_t now = now_ns();
  retry_at_ns_ = now + static_cast<uint64_t>(delays[index]) * 1000000000;
  state_.reader_state = "retrying";
  state_.last_error = error > 0 ? error : EIO;
  state_.observed_boottime_ns = now;
  monitor_.set_native_journal(state_);
}

int Reader::timeout_ms() const {
  if (!enabled_ || fd_ >= 0)
    return -1;
  const uint64_t now = now_ns();
  if (retry_at_ns_ <= now)
    return 0;
  return static_cast<int>(
      std::min<uint64_t>((retry_at_ns_ - now + 999999) / 1000000, INT_MAX));
}

bool Reader::validate(const yz_exit_history_header &header,
                      const yz_exit_history_record *records, size_t length,
                      crash::NativeExitJournalState *next) const {
  if (header.version != YZ_EXIT_HISTORY_VERSION ||
      header.record_size != sizeof(yz_exit_history_record) ||
      header.count > YZ_EXIT_HISTORY_BATCH_MAX ||
      length != sizeof(header) + header.count * sizeof(*records) ||
      header.epoch == 0 || header.observer_active > 1 || header.reserved != 0 ||
      (header.flags & ~(YZ_EXIT_HISTORY_F_RESET | YZ_EXIT_HISTORY_F_OVERFLOW)))
    return false;
  const bool changed = state_.epoch != 0 && header.epoch != state_.epoch;
  if (changed != ((header.flags & YZ_EXIT_HISTORY_F_RESET) != 0))
    return false;
  auto result = changed ? crash::NativeExitJournalState{} : state_;
  if (result.epoch == 0 && header.coverage_generation != 0)
    result.coverage_generation = 1;
  if (header.coverage_generation < result.coverage_generation)
    return false;
  uint64_t sequence = result.cursor;
  if (header.flags & YZ_EXIT_HISTORY_F_OVERFLOW) {
    if (sequence == UINT64_MAX || header.lost_first != sequence + 1 ||
        header.lost_last < header.lost_first ||
        header.lost_last == UINT64_MAX ||
        header.lost_last + 1 != header.oldest_sequence)
      return false;
    result.missing_total = add_saturated(
        result.missing_total, header.lost_last - header.lost_first + 1);
    result.last_gap_first = header.lost_first;
    result.last_gap_last = header.lost_last;
    sequence = header.lost_last;
  } else if (header.lost_first || header.lost_last) {
    return false;
  }
  if ((header.oldest_sequence == 0) != (header.newest_sequence == 0) ||
      header.oldest_sequence > header.newest_sequence ||
      header.next_sequence > header.newest_sequence)
    return false;
  const uint64_t now = now_ns();
  for (uint32_t index = 0; index < header.count; ++index) {
    const auto &record = records[index];
    const auto &event = record.event;
    if (sequence == UINT64_MAX || record.sequence != sequence + 1 ||
        record.sequence < header.oldest_sequence ||
        record.sequence > header.newest_sequence ||
        event.event.type != YZ_EV_TARGET_EXIT || event.event.pid == 0 ||
        event.event.pid > INT32_MAX || event.generation == 0 ||
        event.start_boottime == 0 ||
        event.observed_boottime < event.start_boottime ||
        event.observed_boottime > now || event.event.appid > UINT16_MAX ||
        (event.event.appid & 0x7f) == 0x7f ||
        (event.kind != YZ_RUNTIME_KIND_NATIVE &&
         event.kind != YZ_RUNTIME_KIND_ZYGOTE) ||
        (event.abi != YZ_RUNTIME_ABI_32 && event.abi != YZ_RUNTIME_ABI_64) ||
        record.injection_state > YZ_RUNTIME_STATE_SAFEMODE ||
        !memchr(record.process, '\0', sizeof(record.process)) ||
        !memchr(record.target, '\0', sizeof(record.target)))
      return false;
    sequence = record.sequence;
  }
  if (sequence != header.next_sequence ||
      (header.count == 0 && sequence < header.newest_sequence))
    return false;
  const uint64_t coverage_delta =
      header.coverage_generation - result.coverage_generation;
  result.coverage_interruptions = add_saturated(
      result.coverage_interruptions,
      (coverage_delta / 2) +
          ((coverage_delta & 1) && result.observer_active ? 1 : 0));
  result.available = true;
  result.epoch = header.epoch;
  result.cursor = header.next_sequence;
  result.coverage_generation = header.coverage_generation;
  result.observer_active = header.observer_active != 0;
  result.reader_state = "active";
  result.last_error = 0;
  result.observed_boottime_ns = now;
  *next = std::move(result);
  return true;
}

void Reader::drain(RuntimeReader runtime, ExitHandler on_exit) {
  if (fd_ < 0)
    return;
  struct Batch {
    yz_exit_history_header header;
    std::array<yz_exit_history_record, YZ_EXIT_HISTORY_BATCH_MAX> records;
  } batch{};
  RuntimeSnapshot snapshot;
  bool have_snapshot = false;
  bool changed = false;
  for (size_t page = 0; page < 4; ++page) {
    ssize_t length;
    do {
      length = read(fd_, &batch, sizeof(batch));
    } while (length < 0 && errno == EINTR);
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (length <= 0) {
      fail(length == 0 ? EPIPE : errno);
      changed = true;
      break;
    }
    crash::NativeExitJournalState next;
    if (static_cast<size_t>(length) < sizeof(batch.header) ||
        !validate(batch.header, batch.records.data(),
                  static_cast<size_t>(length), &next)) {
      fail(EPROTO);
      changed = true;
      break;
    }
    for (uint32_t index = 0; index < batch.header.count; ++index) {
      const auto &record = batch.records[index];
      const auto &event = record.event;
      if (event.kind != YZ_RUNTIME_KIND_NATIVE || event.abi != abi_)
        continue;
      if (!have_snapshot) {
        snapshot = runtime ? runtime() : RuntimeSnapshot{};
        monitor_.begin_native_batch();
        have_snapshot = true;
      }
      auto context = native_context(event, snapshot);
      if (context.target_type != record.target_type ||
          context.target != record.target) {
        context.modules.clear();
        context.modules_observed = false;
      }
      context.process = record.process;
      context.target = record.target;
      context.target_type = record.target_type;
      context.state = record.injection_state;
      if (on_exit)
        on_exit(event, batch.header.epoch);
      monitor_.on_native_exit(event, context, batch.header.epoch,
                              record.sequence, true);
    }
    state_ = std::move(next);
    epoch_confirmed_ = true;
    failures_ = 0;
    monitor_.set_native_journal(state_);
    changed = true;
  }
  if (changed)
    monitor_.flush_native();
}

} // namespace yukizygisk::history
