#pragma once

#include "crash_evidence.hpp"

#include <charconv>
#include <cmath>

namespace yukizygisk::crash {

inline bool native_exit_u64(const json::Value &value, uint64_t *result) {
  if (!value.is_string() || value.s.empty())
    return false;
  const auto parsed =
      std::from_chars(value.s.data(), value.s.data() + value.s.size(), *result);
  return parsed.ec == std::errc{} &&
         parsed.ptr == value.s.data() + value.s.size();
}

inline uint64_t native_exit_time(const json::Value &value) {
  uint64_t result = 0;
  return native_exit_u64(value, &result) ? result : 0;
}

struct NativeExitJournalState {
  bool available = false;
  uint64_t epoch = 0;
  uint64_t cursor = 0;
  uint64_t missing_total = 0;
  uint64_t last_gap_first = 0;
  uint64_t last_gap_last = 0;
  uint64_t coverage_generation = 0;
  uint64_t coverage_interruptions = 0;
  bool observer_active = false;
  uint64_t observed_boottime_ns = 0;
  std::string reader_state = "unknown";
  int last_error = 0;
};

inline json::Value
native_exit_journal_json(const NativeExitJournalState &state) {
  json::Value value = json::Value::object();
  value["available"] = state.available;
  value["epoch"] = std::to_string(state.epoch);
  value["cursor"] = std::to_string(state.cursor);
  value["missing_total"] = std::to_string(state.missing_total);
  value["last_gap_first"] = std::to_string(state.last_gap_first);
  value["last_gap_last"] = std::to_string(state.last_gap_last);
  value["coverage_generation"] = std::to_string(state.coverage_generation);
  value["coverage_interruptions"] =
      std::to_string(state.coverage_interruptions);
  value["observer_active"] = state.observer_active;
  value["observed_boottime_ns"] = std::to_string(state.observed_boottime_ns);
  value["reader_state"] = state.reader_state;
  value["last_error"] = state.last_error;
  return value;
}

inline bool parse_native_exit_journal(const json::Value &value,
                                      NativeExitJournalState *result) {
  NativeExitJournalState state;
  if (!value.is_object() || !value.at("available").is_bool() ||
      !value.at("observer_active").is_bool() ||
      !native_exit_u64(value.at("epoch"), &state.epoch) ||
      !native_exit_u64(value.at("cursor"), &state.cursor) ||
      !native_exit_u64(value.at("missing_total"), &state.missing_total) ||
      !native_exit_u64(value.at("last_gap_first"), &state.last_gap_first) ||
      !native_exit_u64(value.at("last_gap_last"), &state.last_gap_last) ||
      !native_exit_u64(value.at("coverage_generation"),
                       &state.coverage_generation) ||
      !native_exit_u64(value.at("coverage_interruptions"),
                       &state.coverage_interruptions) ||
      !native_exit_u64(value.at("observed_boottime_ns"),
                       &state.observed_boottime_ns))
    return false;
  state.available = value.at("available").b;
  state.observer_active = value.at("observer_active").b;
  state.reader_state = value.at("reader_state").string_or("");
  const auto &error = value.at("last_error");
  if ((state.reader_state != "active" && state.reader_state != "retrying" &&
       state.reader_state != "unsupported") ||
      !error.is_number() || !std::isfinite(error.n) || error.n < 0 ||
      error.n > INT32_MAX || std::floor(error.n) != error.n ||
      (!state.epoch && state.cursor) ||
      (state.last_gap_first == 0) != (state.last_gap_last == 0) ||
      state.last_gap_first > state.last_gap_last ||
      state.last_gap_last > state.cursor)
    return false;
  state.last_error = static_cast<int>(error.n);
  *result = std::move(state);
  return true;
}

inline bool native_exit_number(const json::Value &value, uint32_t maximum) {
  return value.is_number() && std::isfinite(value.n) && value.n >= 0 &&
         value.n <= maximum && std::floor(value.n) == value.n;
}

inline bool native_exit_outcome(const json::Value &value) {
  return value.is_string() && (value.s == "injected" || value.s == "failed" ||
                               value.s == "crashed" || value.s == "unknown");
}

inline std::string native_exit_key(uint32_t pid, uint32_t generation,
                                   uint64_t start_ns) {
  return std::to_string(pid) + ":" + std::to_string(generation) + ":" +
         std::to_string(start_ns);
}

inline bool valid_native_exit(const json::Value &item, uint8_t abi) {
  const auto &pid = item.at("pid");
  const auto &generation = item.at("generation");
  const uint64_t start = native_exit_time(item.at("start_boottime_ns"));
  const uint64_t observed = native_exit_time(item.at("observed_boottime_ns"));
  if (!item.is_object() || !native_exit_number(pid, INT32_MAX) || pid.n == 0 ||
      !native_exit_number(generation, UINT32_MAX) || generation.n == 0 ||
      item.at("kind").string_or("") != "native" ||
      !native_exit_number(item.at("abi_id"), 2) ||
      item.at("abi_id").u32_or(0) != abi || !start || observed < start ||
      item.at("key").string_or("") !=
          native_exit_key(pid.u32_or(0), generation.u32_or(0), start) ||
      !native_exit_number(item.at("wait_status"), UINT16_MAX) ||
      !native_exit_outcome(item.at("injection_state")) ||
      !item.at("process").is_string() || item.at("process").s.size() > 256 ||
      !item.at("target").is_string() || item.at("target").s.size() > 256 ||
      !item.at("modules").is_array() ||
      item.at("modules").a.size() > kMaxEvidence ||
      !item.at("tombstone_candidates").is_array() ||
      item.at("tombstone_candidates").a.size() > kMaxEvidence)
    return false;
  if (item.contains("journal_epoch") || item.contains("journal_sequence")) {
    if (!native_exit_time(item.at("journal_epoch")) ||
        !native_exit_time(item.at("journal_sequence")))
      return false;
  }
  const auto reason = item.at("exit_reason").string_or("");
  const auto phase = item.at("phase").string_or("");
  if ((reason != "normal" && reason != "exit_error" && reason != "signal") ||
      (phase != "before_load_report" && phase != "after_load_report" &&
       phase != "injection_failed" && phase != "unknown"))
    return false;
  for (const auto &module : item.at("modules").a) {
    if (!module.is_object() || !valid_id(module.at("module").string_or("")) ||
        !native_exit_outcome(module.at("injection_state")))
      return false;
  }
  for (const auto &candidate : item.at("tombstone_candidates").a) {
    if (!candidate.is_object() || !candidate.at("identity").is_string() ||
        candidate.at("identity").s.size() > 256 ||
        native_exit_time(candidate.at("completed_boottime_ns")) < start ||
        !candidate.at("tombstone").is_string() ||
        candidate.at("tombstone").s.size() > 64 ||
        !candidate.at("frames").is_array() ||
        candidate.at("frames").a.size() > kMaxEvidence)
      return false;
    for (const auto &frame : candidate.at("frames").a) {
      if (!frame.is_object() || !valid_id(frame.at("module").string_or("")) ||
          !frame.at("image").is_string() || frame.at("image").s.size() > 4096 ||
          !frame.at("frame").is_string() || frame.at("frame").s.size() > 4096)
        return false;
    }
  }
  return true;
}

struct NativeExitDocument {
  json::Value exits = json::Value::array();
  NativeExitJournalState journal;
  bool valid = false;
  bool journal_valid = false;
};

inline NativeExitDocument
read_native_exit_document(const std::string &directory, uint8_t abi) {
  NativeExitDocument result;
  const auto boot = boot_id();
  if ((abi != 1 && abi != 2) || !current_boot(directory, boot))
    return result;
  std::string text;
  const char *name = abi == 2 ? "/native_exit64.json" : "/native_exit32.json";
  if (!read_bounded(directory + name, kMaxDocument, &text))
    return result;
  const auto document = json::parse(text);
  if (document.at("version").n != 1 ||
      document.at("boot_id").string_or("") != boot ||
      !document.at("exits").is_array())
    return result;
  result.valid = true;
  bool valid_entries = document.at("exits").a.size() <= kMaxEvidence;
  for (const auto &item : document.at("exits").a) {
    if (!valid_native_exit(item, abi)) {
      valid_entries = false;
      continue;
    }
    if (result.exits.a.size() < kMaxEvidence)
      result.exits.push_back(item);
  }
  result.journal_valid =
      valid_entries &&
      parse_native_exit_journal(document.at("journal"), &result.journal);
  if (result.journal_valid) {
    for (const auto &item : result.exits.a) {
      if (native_exit_time(item.at("journal_epoch")) == result.journal.epoch &&
          native_exit_time(item.at("journal_sequence")) > result.journal.cursor)
        result.journal_valid = false;
    }
  }
  if (!result.journal_valid)
    result.journal = {};
  return result;
}

inline json::Value read_native_exits(const std::string &directory) {
  json::Value result = json::Value::array();
  for (uint8_t abi : {uint8_t{1}, uint8_t{2}})
    for (const auto &item : read_native_exit_document(directory, abi).exits.a)
      result.push_back(item);
  return result;
}

inline json::Value
read_native_exit_journal_status(const std::string &directory) {
  json::Value result = json::Value::array();
  for (uint8_t abi : {uint8_t{1}, uint8_t{2}}) {
    const auto document = read_native_exit_document(directory, abi);
    if (!document.journal_valid)
      continue;
    auto value = native_exit_journal_json(document.journal);
    value["abi_id"] = static_cast<int>(abi);
    value["abi"] = abi == 2 ? "arm64-v8a" : "armeabi-v7a";
    result.push_back(value);
  }
  return result;
}

} // namespace yukizygisk::crash
