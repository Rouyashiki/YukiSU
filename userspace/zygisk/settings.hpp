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

namespace detail {

class ScopedFd final {
public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    const int saved_errno = errno;
    if (fd_ >= 0)
      close(fd_);
    errno = saved_errno;
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
  int get() const { return fd_; }
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

private:
  int fd_;
};

inline std::string directory_of(const std::string &path) {
  const auto separator = path.find_last_of('/');
  return separator == std::string::npos ? "."
         : separator == 0               ? "/"
                                        : path.substr(0, separator);
}

inline int lock_config(const std::string &path) {
  const auto directory = directory_of(path);
  if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
    return -1;
  ScopedFd lock(open((path + ".lock").c_str(),
                     O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                     0600));
  if (lock.get() < 0)
    return -1;
  struct stat status{};
  if (fstat(lock.get(), &status) != 0)
    return -1;
  if (!S_ISREG(status.st_mode) || status.st_uid != 0 || status.st_nlink != 1) {
    errno = EPERM;
    return -1;
  }
  int result;
  do {
    result = flock(lock.get(), LOCK_EX);
  } while (result != 0 && errno == EINTR);
  return result == 0 ? lock.release() : -1;
}

enum class ReadResult { Ready, Invalid, Error };

// Distinguish malformed content from unreadable/unsafe files. Only the former
// may be replaced, and callers must re-read under the writer lock first.
inline ReadResult read_once(const std::string &path, json::Value *root) {
  *root = defaults();
  std::string text;
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0)
    return errno == ENOENT ? ReadResult::Ready : ReadResult::Error;
  if (!crash::read_bounded(path, crash::kMaxDocument, &text))
    return ReadResult::Error;
  const auto saved = json::parse(text);
  if (!saved.is_object())
    return ReadResult::Invalid;
  for (const auto &[key, value] : saved.as_object())
    (*root)[key] = value;
  return ReadResult::Ready;
}

// The same lock protects repairs and ordinary updates. Flush the replacement
// before rename so another crash cannot expose a partially written document.
inline bool write_atomic(const std::string &path, const json::Value &root) {
  ScopedFd directory(
      open(directory_of(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0)
    return false;
  const auto text = json::dump(root);
  std::string temporary = path + ".tmp.XXXXXX";
  ScopedFd file(mkstemp(temporary.data()));
  if (file.get() < 0)
    return false;
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count =
        write(file.get(), text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (count == 0)
        errno = EIO;
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (offset != text.size() || fsync(file.get()) != 0 ||
      rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved_errno = errno;
    (void)unlink(temporary.c_str());
    errno = saved_errno;
    return false;
  }
  return fsync(directory.get()) == 0;
}

} // namespace detail

inline bool read(const std::string &path, json::Value *root) {
  auto result = detail::read_once(path, root);
  if (result != detail::ReadResult::Invalid)
    return result == detail::ReadResult::Ready;
  detail::ScopedFd lock(detail::lock_config(path));
  if (lock.get() < 0)
    return false;
  result = detail::read_once(path, root);
  if (result != detail::ReadResult::Invalid)
    return result == detail::ReadResult::Ready;
  return detail::write_atomic(path, *root);
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
  detail::ScopedFd lock(detail::lock_config(path));
  if (lock.get() < 0)
    return false;
  json::Value root;
  if (detail::read_once(path, &root) == detail::ReadResult::Error)
    return false;
  for (const auto &[key, value] : changes.as_object())
    root[key] = value;
  return detail::write_atomic(path, root);
}

} // namespace yukizygisk::settings
