#pragma once

#include "crash_evidence.hpp"

#include <charconv>
#include <cmath>

namespace yukizygisk::crash {

inline uint64_t native_exit_time(const json::Value &value) {
  if (!value.is_string() || value.s.empty())
    return 0;
  uint64_t result = 0;
  const auto parsed =
      std::from_chars(value.s.data(), value.s.data() + value.s.size(), result);
  return parsed.ec == std::errc{} &&
                 parsed.ptr == value.s.data() + value.s.size()
             ? result
             : 0;
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

inline json::Value read_native_exits(const std::string &directory) {
  json::Value result = json::Value::array();
  const auto boot = boot_id();
  if (!current_boot(directory, boot))
    return result;
  for (uint8_t abi : {uint8_t{1}, uint8_t{2}}) {
    std::string text;
    const char *name = abi == 2 ? "/native_exit64.json" : "/native_exit32.json";
    if (!read_bounded(directory + name, kMaxDocument, &text))
      continue;
    const auto document = json::parse(text);
    if (document.at("version").n != 1 ||
        document.at("boot_id").string_or("") != boot ||
        !document.at("exits").is_array())
      continue;
    size_t count = 0;
    for (const auto &item : document.at("exits").a) {
      if (++count > kMaxEvidence)
        break;
      if (valid_native_exit(item, abi))
        result.push_back(item);
    }
  }
  return result;
}

} // namespace yukizygisk::crash
