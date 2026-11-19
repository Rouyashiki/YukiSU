#pragma once

#include "../common/json.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace yukizygisk::crash {

inline constexpr size_t kMaxEvidence = 64;
inline constexpr size_t kMaxDocument = size_t{512} * 1024;
inline constexpr uint64_t kWindowNs = 60ULL * 1000000000;

inline std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline bool valid_id(std::string_view value) {
  return !value.empty() && value.size() < 64 && value != "." && value != ".." &&
         std::all_of(
             value.begin(), value.end(),
             [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                      ch == '.';
             });
}

inline uint32_t decimal_prefix(std::string_view value) {
  uint64_t result = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9')
      break;
    result = result * 10 + static_cast<unsigned>(ch - '0');
    if (result > INT32_MAX)
      return 0;
  }
  return static_cast<uint32_t>(result);
}

struct Frame {
  std::string module;
  std::string path;
  std::string text;
  uint32_t index = UINT32_MAX;
};

struct Tombstone {
  uint32_t pid = 0;
  uint32_t ppid = 0;
  uint32_t tid = 0;
  uint32_t uid = UINT32_MAX;
  uint32_t signal = 0;
  uint8_t abi = 0;
  std::string process;
  std::string thread_name;
  std::string timestamp;
  std::vector<Frame> frames;
};

inline bool frame_line(std::string_view line, Frame *frame) {
  if (line.empty() || line.front() != '#')
    return false;
  line.remove_prefix(1);
  size_t count = 0;
  while (count < line.size() && line[count] >= '0' && line[count] <= '9')
    ++count;
  if (count == 0 || count == line.size() ||
      (line[count] != ' ' && line[count] != '\t'))
    return false;
  const auto index = decimal_prefix(line.substr(0, count));
  if (index == 0 &&
      line.substr(0, count).find_first_not_of('0') != std::string_view::npos)
    return false;
  line = trim(line.substr(count));
  if (line.substr(0, 3) != "pc ")
    return false;
  line = trim(line.substr(3));
  count = line.find_first_not_of("0123456789abcdefABCDEF");
  if (count == 0 || count == std::string_view::npos ||
      (line[count] != ' ' && line[count] != '\t'))
    return false;
  line = trim(line.substr(count));
  constexpr std::string_view prefix = "/data/adb/modules/";
  if (line.substr(0, prefix.size()) != prefix)
    return false;
  const auto path = line.substr(0, line.find_first_of(" \t("));
  const auto separator = path.find('/', prefix.size());
  if (separator == std::string_view::npos || separator + 1 >= path.size())
    return false;
  const auto id = path.substr(prefix.size(), separator - prefix.size());
  if (!valid_id(id))
    return false;
  frame->module = std::string(id);
  frame->path = std::string(path);
  frame->index = index;
  return true;
}

inline Tombstone parse_tombstone(std::string_view text) {
  Tombstone result;
  bool backtrace = false;
  for (size_t begin = 0; begin < text.size();) {
    size_t end = text.find('\n', begin);
    if (end == std::string_view::npos)
      end = text.size();
    const auto line = trim(text.substr(begin, end - begin));
    begin = end == text.size() ? end : end + 1;
    if (line.size() > 4096)
      continue;
    if (backtrace) {
      if (line.empty() || line == "backtrace:")
        break;
      Frame frame;
      if (result.frames.size() < kMaxEvidence && frame_line(line, &frame)) {
        frame.text = std::string(line);
        if (std::none_of(
                result.frames.begin(), result.frames.end(),
                [&](const Frame &old) { return old.path == frame.path; }))
          result.frames.push_back(std::move(frame));
      }
      continue;
    }
    if (line == "backtrace:") {
      backtrace = true;
    } else if (line.substr(0, 8) == "Cmdline:") {
      result.process = std::string(trim(line.substr(8)).substr(0, 256));
    } else if (line.substr(0, 10) == "Timestamp:") {
      result.timestamp = std::string(trim(line.substr(10)).substr(0, 128));
    } else if (line == "ABI: 'arm64'") {
      result.abi = 2;
    } else if (line == "ABI: 'arm'") {
      result.abi = 1;
    } else if (line.substr(0, 7) == "signal ") {
      result.signal = decimal_prefix(line.substr(7));
    } else if (line.substr(0, 4) == "uid:") {
      result.uid = decimal_prefix(trim(line.substr(4)));
    } else if (line.substr(0, 4) == "pid:") {
      result.pid = decimal_prefix(trim(line.substr(4)));
      const auto parent = line.find("ppid:");
      if (parent != std::string_view::npos)
        result.ppid = decimal_prefix(trim(line.substr(parent + 5)));
      const auto thread = line.find("tid:");
      if (thread != std::string_view::npos)
        result.tid = decimal_prefix(trim(line.substr(thread + 4)));
      const auto name = line.find("name:");
      const auto command = line.find(">>>");
      if (name != std::string_view::npos && command != std::string_view::npos &&
          command > name + 5)
        result.thread_name =
            std::string(trim(line.substr(name + 5, command - name - 5)));
      if (result.process.empty()) {
        const auto first = line.find(">>> ");
        const auto last = line.find(" <<<");
        if (first != std::string_view::npos && last != std::string_view::npos &&
            last > first + 4)
          result.process = std::string(
              line.substr(first + 4, last - first - 4).substr(0, 256));
      }
    }
  }
  return result;
}

inline bool recent_mtime(int64_t now, int64_t modified) {
  return now >= 0 && modified >= 0 && modified <= now && now - modified <= 60;
}

struct Exit {
  uint32_t pid = 0;
  uint32_t generation = 0;
  uint32_t status = 0;
  uint8_t abi = 0;
  uint64_t start_ns = 0;
  uint64_t observed_ns = 0;
};

inline bool matches(const Exit &exit, const Tombstone &tombstone,
                    uint64_t completed_ns) {
  const uint64_t distance = completed_ns > exit.observed_ns
                                ? completed_ns - exit.observed_ns
                                : exit.observed_ns - completed_ns;
  return exit.pid != 0 && exit.generation != 0 && exit.start_ns != 0 &&
         exit.observed_ns >= exit.start_ns && completed_ns >= exit.start_ns &&
         (exit.status & 0x7f) != 0 && (exit.status & 0x7f) != 15 &&
         tombstone.signal != 0 && tombstone.pid != 0 &&
         !tombstone.process.empty() && tombstone.abi == exit.abi &&
         (tombstone.pid == exit.pid || tombstone.ppid == exit.pid) &&
         distance <= kWindowNs;
}

enum class FileSource {
  RootOwned,
  RootPrivate,
  RootPrivateSingleLink,
  Tombstone
};

inline bool read_bounded(const std::string &path, size_t limit,
                         std::string *text,
                         FileSource source = FileSource::RootOwned) {
  const int fd =
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat before{};
  // Android tombstones belong to AID_TOMBSTONED; saved state stays root-only.
  constexpr uid_t kTombstonedUid = 1058;
  const bool single_link = source == FileSource::RootPrivateSingleLink;
  const bool private_file = single_link || source == FileSource::RootPrivate;
  int error = 0;
  if (fstat(fd, &before) != 0)
    error = errno;
  else if (single_link && before.st_nlink != 1)
    error = before.st_nlink == 0 ? EAGAIN : EMLINK;
  else if (!S_ISREG(before.st_mode) ||
           (before.st_uid != 0 && !(source == FileSource::Tombstone &&
                                    before.st_uid == kTombstonedUid)) ||
           (private_file && (before.st_mode & 0022)))
    error = EPERM;
  else if (before.st_size < 0 || static_cast<uint64_t>(before.st_size) > limit)
    error = EFBIG;
  text->clear();
  char buffer[4096];
  while (!error) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count == 0)
      break;
    if (count < 0 || text->size() + static_cast<size_t>(count) > limit) {
      error = count < 0 ? errno : EAGAIN;
      break;
    }
    text->append(buffer, static_cast<size_t>(count));
  }
  struct stat after{};
  if (!error) {
    if (fstat(fd, &after) != 0)
      error = errno;
    else if (single_link && after.st_nlink != 1)
      error = after.st_nlink == 0 ? EAGAIN : EMLINK;
    else if (before.st_uid != after.st_uid || before.st_size != after.st_size ||
             before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
             before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
             (private_file &&
              (before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
               before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
               (after.st_mode & 0022))))
      error = EAGAIN;
  }
  close(fd);
  if (error) {
    text->clear();
    errno = error;
  }
  return !error;
}

inline std::string boot_id() {
  std::string value;
  if (!read_bounded("/proc/sys/kernel/random/boot_id", 128, &value))
    return {};
  return std::string(trim(value));
}

inline bool current_boot(const std::string &directory, const std::string &id) {
  std::string saved;
  return !id.empty() && read_bounded(directory + "/boot_id", 128, &saved) &&
         trim(saved) == id;
}

inline std::string image_identity(const std::string &path) {
  struct stat status{};
  if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
    return {};
  return std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) +
         ":" + std::to_string(status.st_size) + ":" +
         std::to_string(status.st_mtim.tv_sec) + ":" +
         std::to_string(status.st_mtim.tv_nsec);
}

inline json::Value read_evidence(const std::string &directory) {
  json::Value result = json::Value::array();
  const std::string id = boot_id();
  if (!current_boot(directory, id))
    return result;
  for (const char *name : {"/crash64.json", "/crash32.json"}) {
    std::string text;
    if (!read_bounded(directory + name, kMaxDocument, &text))
      continue;
    const auto root = json::parse(text);
    if (root.at("boot_id").string_or("") != id ||
        !root.at("evidence").is_array())
      continue;
    size_t count = 0;
    for (const auto &item : root.at("evidence").as_array()) {
      if (++count > kMaxEvidence)
        break;
      const std::string image = item.at("image").string_or("");
      const std::string identity = item.at("image_identity").string_or("");
      if (!identity.empty() && image_identity(image) == identity &&
          item.is_object() && valid_id(item.at("module").string_or("")) &&
          !item.at("process").string_or("").empty() &&
          item.at("generation").u32_or(0) != 0)
        result.push_back(item);
    }
  }
  return result;
}

} // namespace yukizygisk::crash
