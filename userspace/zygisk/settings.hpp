#pragma once

#include "crash_evidence.hpp"

#include <cstdlib>
#include <sys/file.h>

namespace yukizygisk::settings {

inline json::Value defaults() {
  auto root = json::Value::object();
  root["yukilinker"] = true;
  root["anonymous_memory"] = true;
  root["early_load"] = false;
  root["denylist_mode"] = 0;
  root["dmesg_log"] = false;
  root["crash_protection"] = true;
  return root;
}

inline bool read(const std::string &path, json::Value *root) {
  *root = defaults();
  std::string text;
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0)
    return errno == ENOENT;
  if (!crash::read_bounded(path, crash::kMaxDocument, &text))
    return false;
  const auto saved = json::parse(text);
  if (!saved.is_object()) {
    errno = EINVAL;
    return false;
  }
  for (const auto &[key, value] : saved.as_object())
    (*root)[key] = value;
  return true;
}

inline bool protection_enabled(const std::string &path) {
  json::Value root;
  return read(path, &root) && root.at("crash_protection").bool_or(false);
}

inline bool update(const std::string &path,
                   const std::vector<std::string> &pairs) {
  if (pairs.empty() || pairs.size() % 2 != 0) {
    errno = EINVAL;
    return false;
  }
  auto changes = json::Value::object();
  const auto schema = defaults();
  for (size_t index = 0; index < pairs.size(); index += 2) {
    const auto &key = pairs[index];
    const auto &value = pairs[index + 1];
    if (schema.at(key).is_bool() && (value == "true" || value == "false")) {
      changes[key] = value == "true";
    } else if (key == "denylist_mode" &&
               (value == "0" || value == "1" || value == "2")) {
      changes[key] = value[0] - '0';
    } else {
      errno = EINVAL;
      return false;
    }
  }
  const auto separator = path.find_last_of('/');
  if (separator == std::string::npos) {
    errno = EINVAL;
    return false;
  }
  const auto directory = path.substr(0, separator);
  if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
    return false;
  const int lock =
      open((path + ".lock").c_str(),
           O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (lock < 0)
    return false;
  struct stat status{};
  bool ok = fstat(lock, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == 0 && status.st_nlink == 1 &&
            flock(lock, LOCK_EX) == 0;
  json::Value root;
  if (ok)
    ok = read(path, &root);
  if (ok) {
    for (const auto &[key, value] : changes.as_object())
      root[key] = value;
    const auto text = json::dump(root);
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    ok = fd >= 0;
    if (fd >= 0) {
      size_t offset = 0;
      while (offset < text.size()) {
        const ssize_t count =
            write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          break;
        offset += static_cast<size_t>(count);
      }
      ok = offset == text.size() && fsync(fd) == 0;
      close(fd);
      if (ok)
        ok = rename(temporary.c_str(), path.c_str()) == 0;
      if (!ok)
        (void)unlink(temporary.c_str());
      else {
        const int dir =
            open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir >= 0) {
          (void)fsync(dir);
          close(dir);
        }
      }
    }
  }
  const int saved_errno = errno;
  close(lock);
  errno = saved_errno;
  return ok;
}

} // namespace yukizygisk::settings
