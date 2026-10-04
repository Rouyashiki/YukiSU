#include "crash_monitor.hpp"

#include <ctime>
#include <dirent.h>
#include <sys/inotify.h>

#include <array>
#include <charconv>
#include <cstring>
#include <utility>

namespace yukizygisk::crash {
namespace {
constexpr uint64_t kRetryNs = 5ULL * 1000000000;
constexpr size_t kMaxTombstone = size_t{4} * 1024 * 1024;

uint64_t now_ns() {
  timespec now{};
  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0)
    return 0;
  return (static_cast<uint64_t>(now.tv_sec) * 1000000000) +
         static_cast<uint64_t>(now.tv_nsec);
}

bool tombstone_name(std::string_view name) {
  constexpr std::string_view prefix = "tombstone_";
  return name.size() > prefix.size() && name.size() < 64 &&
         name.substr(0, prefix.size()) == prefix &&
         std::all_of(
             name.begin() + prefix.size(), name.end(),
             [](char ch) { return ch >= '0' && ch <= '9'; });
}

std::string stamp_identity(const struct stat &status) {
  return std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) +
         ":" + std::to_string(status.st_size) + ":" +
         std::to_string(status.st_mtim.tv_sec) + ":" +
         std::to_string(status.st_mtim.tv_nsec);
}

bool write_all(int fd, const std::string &text) {
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

const char *native_outcome(uint8_t state) {
  switch (state) {
  case YZ_RUNTIME_STATE_INJECTED:
    return "injected";
  case YZ_RUNTIME_STATE_FAILED:
    return "failed";
  case YZ_RUNTIME_STATE_SAFEMODE:
    return "crashed";
  default:
    return "unknown";
  }
}

const char *native_phase(uint8_t state) {
  switch (state) {
  case YZ_RUNTIME_STATE_DETECTED:
  case YZ_RUNTIME_STATE_REDIRECTED:
    return "before_load_report";
  case YZ_RUNTIME_STATE_INJECTED:
    return "after_load_report";
  case YZ_RUNTIME_STATE_FAILED:
  case YZ_RUNTIME_STATE_SAFEMODE:
    return "injection_failed";
  default:
    return "unknown";
  }
}

bool native_matches(const Exit &event, const Tombstone &tombstone,
                    uint64_t completed_ns) {
  return tombstone.pid == event.pid &&
         tombstone.signal == (event.status & 0x7f) &&
         matches(event, tombstone, completed_ns);
}
} // namespace

Monitor::~Monitor() {
  if (notify_ >= 0)
    close(notify_);
}

void Monitor::log(const std::string &message) const {
  if (logger_ != nullptr)
    logger_(message.c_str());
}

bool Monitor::Stamp::operator==(const Stamp &other) const {
  return device == other.device && inode == other.inode && size == other.size &&
         seconds == other.seconds && nanos == other.nanos;
}

void Monitor::set_modules(std::vector<Module> modules) {
  for (auto &module : modules)
    module.identity = image_identity(module.path);
  modules_ = std::move(modules);
}

void Monitor::start(std::string directory, uint8_t abi) {
  directory_ = std::move(directory);
  abi_ = abi;
  boot_ = boot_id();
  protection_.start(directory_, abi_);
  const auto saved = read_evidence(directory_);
  for (const auto &item : saved.as_array()) {
    if (item.at("abi_id").u32_or(0) == abi_ &&
        evidence_.a.size() < kMaxEvidence)
      evidence_.push_back(item);
  }
  for (const auto &item : read_native_exits(directory_).a) {
    if (item.at("abi_id").u32_or(0) != abi_)
      continue;
    native_evidence_.push_back(item);
    native_exits_.push_back(
        {item.at("pid").u32_or(0), item.at("generation").u32_or(0),
         item.at("wait_status").u32_or(0), abi_,
         native_exit_time(item.at("start_boottime_ns")),
         native_exit_time(item.at("observed_boottime_ns"))});
  }
  watch();
}

void Monitor::watch() {
  retry_ns_ = now_ns() + kRetryNs;
  if (notify_ < 0)
    notify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (notify_ < 0)
    return;
  watch_ = inotify_add_watch(notify_, tombstones_.c_str(),
                             IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF |
                                 IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW);
  if (watch_ < 0)
    return;
  log("watch armed abi=" + std::to_string(abi_));
  // Establish the baseline after arming the watch; existing files are not new
  // evidence.
  stamps_.clear();
  candidates_.clear();
  native_candidates_.clear();
  scan();
}

void Monitor::scan() {
  DIR *directory = opendir(tombstones_.c_str());
  if (directory == nullptr)
    return;
  size_t count = 0;
  while (dirent *entry = readdir(directory)) {
    if (!tombstone_name(entry->d_name))
      continue;
    if (++count > 128)
      break;
    struct stat status{};
    const std::string path = tombstones_ + "/" + entry->d_name;
    if (lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode))
      stamps_[entry->d_name] = {status.st_dev, status.st_ino, status.st_size,
                                status.st_mtim.tv_sec, status.st_mtim.tv_nsec};
  }
  closedir(directory);
}

void Monitor::consume_closed(const std::string &name) {
  if (tombstone_name(name)) {
    consume(name);
    return;
  }
  // O_TMPFILE keeps its #inode event name after tombstoned publishes via
  // linkat.
  if (name.size() < 2 || name.front() != '#')
    return;
  uint64_t inode = 0;
  const auto parsed =
      std::from_chars(name.data() + 1, name.data() + name.size(), inode);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() ||
      inode == 0)
    return;
  DIR *directory = opendir(tombstones_.c_str());
  if (directory == nullptr)
    return;
  size_t count = 0;
  while (dirent *entry = readdir(directory)) {
    if (!tombstone_name(entry->d_name))
      continue;
    if (++count > 128)
      break;
    struct stat status{};
    const std::string path = tombstones_ + "/" + entry->d_name;
    if (lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
        status.st_ino == inode)
      consume(entry->d_name, inode);
  }
  closedir(directory);
}

void Monitor::consume(const std::string &name, uint64_t inode) {
  if (!tombstone_name(name))
    return;
  const std::string path = tombstones_ + "/" + name;
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || (inode != 0 && status.st_ino != inode) ||
      static_cast<uint64_t>(status.st_size) > kMaxTombstone)
    return;
  const Stamp stamp{status.st_dev, status.st_ino, status.st_size,
                    status.st_mtim.tv_sec, status.st_mtim.tv_nsec};
  const auto found = stamps_.find(name);
  if (found != stamps_.end() && found->second == stamp)
    return;
  if (!recent_mtime(time(nullptr), stamp.seconds)) {
    log("tombstone rejected: mtime " + name);
    return;
  }
  std::string text;
  if (!read_bounded(path, kMaxTombstone, &text, FileSource::Tombstone)) {
    log("tombstone rejected: read/identity " + name);
    return;
  }
  struct stat after{};
  if (lstat(path.c_str(), &after) != 0 ||
      stamp_identity(after) != stamp_identity(status))
    return;
  if (stamps_.size() < 128 || found != stamps_.end())
    stamps_[name] = stamp;
  Candidate candidate{name, stamp_identity(status), now_ns(),
                      parse_tombstone(text)};
  if (candidate.tombstone.abi == abi_)
    log("tombstone received: " + name +
        " pid=" + std::to_string(candidate.tombstone.pid) +
        " ppid=" + std::to_string(candidate.tombstone.ppid) +
        " module_frames=" + std::to_string(candidate.tombstone.frames.size()));
  if (candidate.tombstone.abi != abi_)
    return;
  correlate_native(candidate);
  if (native_candidates_.size() == kMaxEvidence)
    native_candidates_.erase(native_candidates_.begin());
  native_candidates_.push_back(candidate);
  if (candidate.tombstone.frames.empty())
    return;
  for (const auto &event : exits_)
    correlate(event, candidate);
  if (candidates_.size() == kMaxEvidence)
    candidates_.erase(candidates_.begin());
  candidates_.push_back(std::move(candidate));
}

void Monitor::correlate(const Exit &event, const Candidate &candidate) {
  const auto &tombstone = candidate.tombstone;
  if (!matches(event, tombstone, candidate.completed_ns))
    return;
  for (const auto &frame : tombstone.frames) {
    const auto module =
        std::find_if(modules_.begin(), modules_.end(), [&](const Module &item) {
          return item.id == frame.module && item.path == frame.path;
        });
    if (module == modules_.end() || module->identity.empty() ||
        image_identity(module->path) != module->identity) {
      log("module identity rejected: " + frame.module);
      continue;
    }
    const std::string key = candidate.identity + ":" + candidate.name + ":" +
                            frame.module + ":" +
                            std::to_string(event.generation);
    if (std::any_of(evidence_.a.begin(), evidence_.a.end(),
                    [&](const json::Value &item) {
                      return item.at("key").string_or("") == key;
                    }))
      continue;
    const bool direct_fault =
        tombstone.pid == event.pid && tombstone.signal == (event.status & 0x7f);
    const bool server_fault = tombstone.ppid == event.pid &&
                              tombstone.uid == 1000 &&
                              (tombstone.process == "system_server" ||
                               (tombstone.tid == tombstone.pid &&
                                tombstone.thread_name == "system_server"));
    if (module->zygisk && frame.index == 0 && (direct_fault || server_fault) &&
        protection_.suspend(frame.module)) {
      log("injection suspended for this boot: " + frame.module +
          " tombstone=" + candidate.name +
          (protection_.dirty() ? " (persistence pending)" : ""));
    }
    json::Value item = json::Value::object();
    item["key"] = key;
    item["module"] = frame.module;
    item["image"] = module->path;
    item["image_identity"] = module->identity;
    item["process"] = tombstone.process;
    item["pid"] = static_cast<double>(tombstone.pid);
    item["zygote_pid"] = static_cast<double>(event.pid);
    item["generation"] = static_cast<double>(event.generation);
    item["abi_id"] = static_cast<int>(abi_);
    item["abi"] = abi_ == 2 ? "arm64-v8a" : "armeabi-v7a";
    item["timestamp"] = tombstone.timestamp;
    item["observed_at"] = static_cast<double>(time(nullptr));
    item["tombstone"] = candidate.name;
    item["frame"] = frame.text;
    if (evidence_.a.size() == kMaxEvidence)
      evidence_.a.erase(evidence_.a.begin());
    evidence_.push_back(item);
    dirty_ = true;
    log("evidence associated: " + frame.module +
        " tombstone=" + candidate.name);
  }
}

void Monitor::on_exit(const yz_zygote_exit_event &event) {
  const uint64_t now = now_ns();
  if (event.event.type != YZ_EV_ZYGOTE_EXIT || event.abi != abi_ ||
      event.event.pid == 0 || event.generation == 0 ||
      event.start_boottime == 0 ||
      event.observed_boottime < event.start_boottime ||
      now < event.observed_boottime ||
      now - event.observed_boottime > kWindowNs ||
      (event.event.appid & 0x7f) == 0 || (event.event.appid & 0x7f) == 15) {
    if (event.abi == abi_)
      log("exit rejected: pid=" + std::to_string(event.event.pid) +
          " status=" + std::to_string(event.event.appid));
    return;
  }
  log("exit accepted: pid=" + std::to_string(event.event.pid) +
      " generation=" + std::to_string(event.generation) +
      " status=" + std::to_string(event.event.appid));
  tick();
  // The dump can finish before the kernel exit notification reaches userspace.
  drain();
  const Exit exit{event.event.pid,      event.generation,
                  event.event.appid,    event.abi,
                  event.start_boottime, event.observed_boottime};
  if (std::any_of(exits_.begin(), exits_.end(), [&](const Exit &old) {
        return old.pid == exit.pid && old.generation == exit.generation &&
               old.start_ns == exit.start_ns;
      }))
    return;
  for (const auto &candidate : candidates_)
    correlate(exit, candidate);
  if (exits_.size() == 16)
    exits_.erase(exits_.begin());
  exits_.push_back(exit);
  save();
}

void Monitor::on_native_exit(const yz_target_exit_event &event,
                             const NativeExitContext &context) {
  const uint64_t now = now_ns();
  if (event.event.type != YZ_EV_TARGET_EXIT ||
      event.kind != YZ_RUNTIME_KIND_NATIVE || event.abi != abi_ ||
      event.event.pid == 0 || event.event.pid > INT32_MAX ||
      event.generation == 0 || event.start_boottime == 0 ||
      event.observed_boottime < event.start_boottime ||
      now < event.observed_boottime ||
      now - event.observed_boottime > kWindowNs ||
      event.event.appid > UINT16_MAX || (event.event.appid & 0x7f) == 0x7f)
    return;
  const std::string key =
      native_exit_key(event.event.pid, event.generation, event.start_boottime);
  if (std::any_of(native_evidence_.a.begin(), native_evidence_.a.end(),
                  [&](const auto &item) {
                    return item.at("key").string_or("") == key;
                  }))
    return;
  drain();
  const uint32_t signal = event.event.appid & 0x7f;
  const uint32_t exit_code = (event.event.appid >> 8) & 0xff;
  const char *reason = signal ? "signal" : exit_code ? "exit_error" : "normal";
  json::Value item = json::Value::object();
  item["key"] = key;
  item["kind"] = "native";
  item["pid"] = static_cast<double>(event.event.pid);
  item["generation"] = static_cast<double>(event.generation);
  item["abi_id"] = static_cast<int>(abi_);
  item["abi"] = abi_ == 2 ? "arm64-v8a" : "armeabi-v7a";
  item["start_boottime_ns"] = std::to_string(event.start_boottime);
  item["observed_boottime_ns"] = std::to_string(event.observed_boottime);
  item["process"] = context.process.substr(0, 256);
  item["target"] = context.target.substr(0, 256);
  item["target_type"] = context.target_type == YZ_NATIVE_TARGET_NAME ? "name"
                        : context.target_type == YZ_NATIVE_TARGET_PATH
                            ? "path"
                            : "unknown";
  item["wait_status"] = static_cast<double>(event.event.appid);
  item["exit_reason"] = reason;
  item["exit_code"] = static_cast<double>(exit_code);
  item["signal"] = static_cast<double>(signal);
  item["core_dumped"] = signal != 0 && (event.event.appid & 0x80) != 0;
  item["injection_state"] = native_outcome(context.state);
  item["phase"] = native_phase(context.state);
  item["modules"] = json::Value::array();
  for (const auto &module : context.modules) {
    if (!valid_id(module.id))
      continue;
    if (item["modules"].a.size() == kMaxEvidence)
      break;
    json::Value outcome = json::Value::object();
    outcome["module"] = module.id;
    outcome["injection_state"] = native_outcome(module.state);
    item["modules"].push_back(outcome);
  }
  item["tombstone_candidates"] = json::Value::array();
  if (native_evidence_.a.size() == kMaxEvidence) {
    native_evidence_.a.erase(native_evidence_.a.begin());
    native_exits_.erase(native_exits_.begin());
  }
  native_evidence_.push_back(item);
  native_exits_.push_back({event.event.pid, event.generation, event.event.appid,
                           event.abi, event.start_boottime,
                           event.observed_boottime});
  native_dirty_ = true;
  log("native exit: pid=" + std::to_string(event.event.pid) + " generation=" +
      std::to_string(event.generation) + " target=" + context.target +
      " reason=" + reason + " phase=" + native_phase(context.state) +
      " injection=" + native_outcome(context.state) +
      " status=" + std::to_string(event.event.appid));
  reconcile_native_candidates();
  for (const auto &candidate : native_candidates_)
    correlate_native(candidate);
  save();
}

void Monitor::correlate_native(const Candidate &candidate) {
  const Exit *match = nullptr;
  unsigned matches_count = 0;
  for (const auto &event : native_exits_) {
    if (native_matches(event, candidate.tombstone, candidate.completed_ns)) {
      match = &event;
      ++matches_count;
    }
  }
  const std::string candidate_key = candidate.identity + ":" + candidate.name;
  const std::string key =
      matches_count == 1 && match != nullptr
          ? native_exit_key(match->pid, match->generation, match->start_ns)
          : std::string{};
  bool already_associated = false;
  // A reused PID can make a previously unique candidate ambiguous.
  for (auto &item : native_evidence_.a) {
    auto &existing = item["tombstone_candidates"].a;
    const auto previous = existing.size();
    existing.erase(std::remove_if(existing.begin(), existing.end(),
                                  [&](const auto &entry) {
                                    if (entry.at("identity").string_or("") !=
                                        candidate_key)
                                      return false;
                                    if (item.at("key").string_or("") == key) {
                                      already_associated = true;
                                      return false;
                                    }
                                    return true;
                                  }),
                   existing.end());
    native_dirty_ |= existing.size() != previous;
  }
  if (matches_count != 1 || match == nullptr) {
    if (matches_count > 1)
      log("native tombstone ambiguous: " + candidate.name +
          " pid=" + std::to_string(candidate.tombstone.pid));
    return;
  }
  if (already_associated)
    return;
  const auto found = std::find_if(
      native_evidence_.a.begin(), native_evidence_.a.end(),
      [&](const auto &item) { return item.at("key").string_or("") == key; });
  if (found == native_evidence_.a.end())
    return;
  json::Value evidence = json::Value::object();
  evidence["identity"] = candidate_key;
  evidence["completed_boottime_ns"] = std::to_string(candidate.completed_ns);
  evidence["tombstone"] = candidate.name;
  evidence["process"] = candidate.tombstone.process;
  evidence["timestamp"] = candidate.tombstone.timestamp;
  evidence["frames"] = json::Value::array();
  for (const auto &frame : candidate.tombstone.frames) {
    const auto module =
        std::find_if(modules_.begin(), modules_.end(), [&](const auto &entry) {
          return entry.id == frame.module && entry.path == frame.path;
        });
    if (module == modules_.end() || module->identity.empty() ||
        frame.path.size() > 4096 ||
        image_identity(module->path) != module->identity ||
        evidence["frames"].a.size() == kMaxEvidence)
      continue;
    json::Value value = json::Value::object();
    value["module"] = frame.module;
    value["image"] = frame.path;
    value["image_identity"] = module->identity;
    value["frame"] = frame.text.substr(0, 4096);
    evidence["frames"].push_back(value);
  }
  auto &entries = (*found)["tombstone_candidates"].a;
  if (entries.size() == kMaxEvidence)
    entries.erase(entries.begin());
  entries.push_back(evidence);
  native_dirty_ = true;
  log("native tombstone candidate: " + candidate.name +
      " pid=" + std::to_string(match->pid) +
      " generation=" + std::to_string(match->generation));
}

void Monitor::reconcile_native_candidates() {
  for (auto &item : native_evidence_.a) {
    auto &candidates = item["tombstone_candidates"].a;
    const auto previous = candidates.size();
    candidates.erase(
        std::remove_if(
            candidates.begin(), candidates.end(),
            [&](const auto &candidate) {
              Tombstone tombstone;
              tombstone.pid = item.at("pid").u32_or(0);
              tombstone.signal = item.at("wait_status").u32_or(0) & 0x7f;
              tombstone.abi = abi_;
              tombstone.process = candidate.at("process").string_or("");
              const auto completed =
                  native_exit_time(candidate.at("completed_boottime_ns"));
              unsigned matches_count = 0;
              const Exit *match = nullptr;
              for (const auto &event : native_exits_) {
                if (native_matches(event, tombstone, completed)) {
                  match = &event;
                  ++matches_count;
                }
              }
              return matches_count != 1 || match == nullptr ||
                     native_exit_key(match->pid, match->generation,
                                     match->start_ns) !=
                         item.at("key").string_or("");
            }),
        candidates.end());
    native_dirty_ |= candidates.size() != previous;
  }
}

void Monitor::drain() {
  if (notify_ < 0)
    return;
  alignas(inotify_event) std::array<char, 8192> buffer{};
  for (unsigned batch = 0; batch < 8; ++batch) {
    const ssize_t size = read(notify_, buffer.data(), buffer.size());
    if (size < 0 && errno == EINTR)
      continue;
    if (size <= 0)
      break;
    size_t offset = 0;
    while (offset + sizeof(inotify_event) <= static_cast<size_t>(size)) {
      const auto *event =
          reinterpret_cast<const inotify_event *>(buffer.data() + offset);
      const size_t remaining =
          static_cast<size_t>(size) - offset - sizeof(*event);
      if (event->len > remaining)
        break;
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        candidates_.clear();
        native_candidates_.clear();
        exits_.clear();
        scan();
      } else if (event->wd == watch_) {
        if ((event->mask & (IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF)) != 0) {
          close(notify_);
          notify_ = -1;
          watch_ = -1;
          exits_.clear();
          candidates_.clear();
          native_candidates_.clear();
          retry_ns_ = now_ns() + kRetryNs;
        } else if ((event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) != 0 &&
                   event->len > 0) {
          const size_t length = strnlen(event->name, event->len);
          if (length < event->len) {
            const std::string name(event->name, length);
            if ((event->mask & IN_CLOSE_WRITE) != 0)
              consume_closed(name);
            else
              consume(name);
          }
        }
      }
      offset += sizeof(*event) + event->len;
    }
  }
  save();
}

void Monitor::tick() {
  if (directory_.empty())
    return;
  protection_.tick();
  const uint64_t now = now_ns();
  if (watch_ < 0 && now >= retry_ns_)
    watch();
  exits_.erase(std::remove_if(exits_.begin(), exits_.end(),
                              [&](const Exit &event) {
                                return now < event.observed_ns ||
                                       now - event.observed_ns > kWindowNs;
                              }),
               exits_.end());
  candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(),
                                   [&](const Candidate &candidate) {
                                     return now < candidate.completed_ns ||
                                            now - candidate.completed_ns >
                                                kWindowNs;
                                   }),
                    candidates_.end());
  native_candidates_.erase(
      std::remove_if(native_candidates_.begin(), native_candidates_.end(),
                     [&](const Candidate &candidate) {
                       return now < candidate.completed_ns ||
                              now - candidate.completed_ns > kWindowNs;
                     }),
      native_candidates_.end());
  if ((dirty_ || native_dirty_) && now >= retry_ns_) {
    retry_ns_ = now + kRetryNs;
    save(true);
  }
}

int Monitor::timeout_ms() const {
  return watch_ < 0 || dirty_ || native_dirty_ || protection_.dirty() ||
                 !exits_.empty() || !candidates_.empty() ||
                 !native_candidates_.empty()
             ? 5000
             : -1;
}

void Monitor::save(bool include_native) {
  if (!dirty_ && !(include_native && native_dirty_))
    return;
  if (!current_boot(directory_, boot_)) {
    log("save deferred: boot identity mismatch");
    return;
  }
  if (dirty_) {
    json::Value root = json::Value::object();
    root["boot_id"] = boot_;
    root["evidence"] = evidence_;
    if (save_document(abi_ == 2 ? "crash64.json" : "crash32.json", root)) {
      dirty_ = false;
      log("evidence saved: records=" + std::to_string(evidence_.a.size()));
    }
  }
  if (include_native && native_dirty_) {
    json::Value root = json::Value::object();
    root["version"] = 1;
    root["boot_id"] = boot_;
    root["exits"] = native_evidence_;
    while (json::dump(root).size() > kMaxDocument) {
      const auto with_candidates =
          std::find_if(native_evidence_.a.begin(), native_evidence_.a.end(),
                       [](const auto &item) {
                         return !item.at("tombstone_candidates").a.empty();
                       });
      if (with_candidates != native_evidence_.a.end()) {
        auto &candidates = (*with_candidates)["tombstone_candidates"].a;
        candidates.erase(candidates.begin());
      } else if (native_evidence_.a.size() > 1) {
        native_evidence_.a.erase(native_evidence_.a.begin());
        native_exits_.erase(native_exits_.begin());
      } else {
        break;
      }
      root["exits"] = native_evidence_;
    }
    if (save_document(abi_ == 2 ? "native_exit64.json" : "native_exit32.json",
                      root))
      native_dirty_ = false;
  }
}

bool Monitor::save_document(const char *name, const json::Value &document) {
  const std::string text = json::dump(document);
  if (text.size() > kMaxDocument)
    return false;
  const int directory =
      open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0)
    return false;
  const std::string temporary =
      std::string(name) + ".tmp." + std::to_string(getpid());
  const int fd = openat(
      directory, temporary.c_str(),
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  bool saved = false;
  if (fd >= 0) {
    struct stat status{};
    bool ok = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
              status.st_uid == 0 && write_all(fd, text) && fsync(fd) == 0;
    close(fd);
    if (ok)
      ok = renameat(directory, temporary.c_str(), directory, name) == 0;
    if (ok) {
      (void)fsync(directory);
      saved = true;
    } else {
      (void)unlinkat(directory, temporary.c_str(), 0);
    }
  }
  close(directory);
  return saved;
}

} // namespace yukizygisk::crash
