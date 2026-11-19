#pragma once

#include "crash_evidence.hpp"
#include "fixed_policy.hpp"
#include "userspace/ksud/src/defs.hpp"

#include <cstdlib>
#include <sys/file.h>

namespace yukizygisk::settings {

inline bool retired_option(const std::string &key) {
  return key == "fixed_zn_domains";
}

inline json::Value defaults() {
  auto root = json::Value::object();
  root["yukilinker"] = true;
  root["anonymous_memory"] = true;
  root["fixed_sepolicy"] = false;
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

enum class ReadResult { Ready, Missing, Rewrite, Invalid, Error };

inline ReadResult read_once(const std::string &path, json::Value *root,
                            std::string *source = nullptr) {
  *root = defaults();
  std::string text;
  std::string &bytes = source ? *source : text;
  bytes.clear();
  if (!crash::read_bounded(path, crash::kMaxDocument, &bytes,
                           crash::FileSource::RootPrivate))
    return errno == ENOENT ? ReadResult::Missing : ReadResult::Error;
  const auto saved = json::parse(bytes);
  if (!saved.is_object())
    return ReadResult::Invalid;
  bool rewrite = false;
  for (const auto &[key, value] : saved.as_object()) {
    if (retired_option(key))
      rewrite = true;
    else
      (*root)[key] = value;
  }
  if (!root->at("fixed_sepolicy").is_bool()) {
    (*root)["fixed_sepolicy"] = false;
    return ReadResult::Invalid;
  }
  return rewrite ? ReadResult::Rewrite : ReadResult::Ready;
}

// The same lock protects repairs and ordinary updates. Flush the replacement
// before rename so another crash cannot expose a partially written document.
inline bool write_text_atomic(const std::string &path, const std::string &text,
                              size_t limit = crash::kMaxDocument) {
  ScopedFd directory(
      open(directory_of(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0)
    return false;
  if (text.size() > limit) {
    errno = EFBIG;
    return false;
  }
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

inline bool remove_policy_file(const std::string &path) {
  ScopedFd dir(open(directory_of(path).c_str(),
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
  if (dir.get() < 0)
    return errno == ENOENT;
  struct stat st{};
  if (fstat(dir.get(), &st) != 0)
    return false;
  if (st.st_uid != 0 || (st.st_mode & 0022)) {
    errno = EPERM;
    return false;
  }
  const auto name = path.substr(path.find_last_of('/') + 1);
  if (unlinkat(dir.get(), name.c_str(), 0) != 0 && errno != ENOENT)
    return false;
  return fsync(dir.get()) == 0;
}

inline bool withdraw_fixed_policy() {
  // The kernel tries watchdog first, then the default path.
  for (const char *directory :
       {ksud::PREINIT_DIR_DEFAULT, ksud::PREINIT_DIR_WATCHDOG}) {
    if (!remove_policy_file(std::string(directory) + "yukizygisk_policy.bin"))
      return false;
  }
  return true;
}

inline constexpr char kPolicyPending[] = ".policy-pending";
inline constexpr size_t kMaxPolicyJournal = 2 * crash::kMaxDocument + 2;

inline bool private_policy_file(const struct stat &st) {
  return S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 0022);
}

inline bool require_private_policy_config(const std::string &path) {
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0)
    return false;
  if (private_policy_file(st))
    return true;
  errno = EPERM;
  return false;
}

enum class BootProfileState { Ready, Missing, Invalid, Aliased, Error };

// The kernel selects the watchdog profile first, falling back only on ENOENT.
inline BootProfileState read_boot_profile(yz_fixed_policy_config *profile) {
  for (const char *directory :
       {ksud::PREINIT_DIR_WATCHDOG, ksud::PREINIT_DIR_DEFAULT}) {
    const std::string path = std::string(directory) + "yukizygisk_policy.bin";
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
      if (errno == ENOENT)
        continue;
      return BootProfileState::Error;
    }
    if (!S_ISREG(st.st_mode))
      return BootProfileState::Invalid;
    std::string bytes;
    if (!crash::read_bounded(path, sizeof(*profile), &bytes,
                             crash::FileSource::RootPrivateSingleLink)) {
      if (errno == EMLINK)
        return BootProfileState::Aliased;
      if (errno == EPERM || errno == EFBIG)
        return BootProfileState::Invalid;
      return BootProfileState::Error;
    }
    if (bytes.size() != sizeof(*profile))
      return BootProfileState::Invalid;
    memcpy(profile, bytes.data(), sizeof(*profile));
    if (profile->version != YZ_FIXED_POLICY_VERSION || profile->enabled > 1 ||
        profile->reserved || profile->count > YZ_FIXED_DOMAIN_MAX)
      return BootProfileState::Invalid;
    for (uint32_t i = 0; i < profile->count; ++i) {
      const auto &name = profile->domains[i];
      if (!memchr(name, '\0', sizeof(name)) || !fixed_policy::domain_name(name))
        return BootProfileState::Invalid;
    }
    return BootProfileState::Ready;
  }
  return BootProfileState::Missing;
}

inline bool profile_matches(BootProfileState state,
                            const yz_fixed_policy_config &actual,
                            const yz_fixed_policy_config &expected) {
  return state == BootProfileState::Missing
             ? !expected.enabled
             : state == BootProfileState::Ready &&
                   memcmp(&actual, &expected, sizeof(expected)) == 0;
}

inline bool boot_matches_config(const json::Value &config, bool *matches) {
  yz_fixed_policy_config expected{}, actual{};
  if (!fixed_policy::encode(config, &expected))
    return false;
  const auto result = read_boot_profile(&actual);
  *matches = profile_matches(result, actual, expected);
  return result != BootProfileState::Error;
}

inline bool recover_policy_config(const std::string &path,
                                  const std::string *requested = nullptr) {
  const auto pending = path + kPolicyPending;
  std::string journal;
  if (!crash::read_bounded(pending, kMaxPolicyJournal, &journal,
                           crash::FileSource::RootPrivateSingleLink)) {
    const int error = errno;
    if (error == ENOENT) {
      if (requested)
        errno = ESTALE;
      return !requested;
    }
    struct stat st{};
    if (lstat(pending.c_str(), &st) != 0 ||
        (private_policy_file(st) && st.st_nlink == 1 &&
         st.st_size <= static_cast<off_t>(kMaxPolicyJournal))) {
      errno = error;
      return false;
    }
    // Untrusted transaction metadata cannot select a profile or replace the
    // live JSON.
    journal.clear();
  }
  // Two NULs mark a baseline that is retired before the JSON rename. Older
  // one-separator journals could survive the rename with their baseline intact.
  const auto separator = journal.find('\0');
  const auto document = journal.substr(0, separator);
  auto baseline = separator == journal.npos
                      ? std::string_view{}
                      : std::string_view(journal).substr(separator + 1);
  const bool prepared = !baseline.empty() && baseline.front() == '\0';
  if (prepared)
    baseline.remove_prefix(1);
  const auto retire = [&](bool committed) {
    if (!remove_policy_file(pending))
      return false;
    if (requested && (!committed || *requested != document)) {
      errno = ESTALE;
      return false;
    }
    return true;
  };
  yz_fixed_policy_config actual{};
  const auto state = read_boot_profile(&actual);
  if (state == BootProfileState::Error)
    return false;
  const auto matches_profile = [&](const std::string &bytes) {
    const auto config = json::parse(bytes);
    yz_fixed_policy_config expected{};
    return config.is_object() && fixed_policy::encode(config, &expected) &&
           profile_matches(state, actual, expected);
  };
  const bool published = document.size() <= crash::kMaxDocument &&
                         baseline.size() <= crash::kMaxDocument &&
                         matches_profile(document);

  std::string current;
  const bool have_current = crash::read_bounded(
      path, crash::kMaxDocument, &current, crash::FileSource::RootPrivate);
  if (!have_current && errno != ENOENT)
    return false;
  if (published && have_current && current == document)
    return retire(true);
  if (published && have_current && prepared && baseline == current) {
    // Once the new JSON is visible, restoring the old bytes must win.
    return write_text_atomic(pending, document) &&
           write_text_atomic(path, document) && retire(true);
  }
  // A restore can change the journal as well as the live JSON. Retire it only
  // after checking the effective boot profile against the settings we keep.
  if (!matches_profile(current)) {
    const bool may_have_grants =
        state != BootProfileState::Missing &&
        (state != BootProfileState::Ready || actual.enabled);
    if (may_have_grants && !fixed_policy::mark_reboot_required())
      return false;
    if (!withdraw_fixed_policy())
      return false;
  }
  return retire(false);
}

inline std::string serialize_config(const json::Value &root,
                                    const std::string &original,
                                    const json::Value *changes = nullptr) {
  auto document = json::dump_object_members(root, original, changes);
  if (document.size() > crash::kMaxDocument) {
    const auto standard = defaults();
    document = json::dump_object_members(root, original, changes, &standard);
  }
  return document;
}

// Callers hold the config writer lock through withdrawal and repair.
inline bool read_locked(const std::string &path, json::Value *root,
                        std::string *source = nullptr) {
  if (!recover_policy_config(path))
    return false;
  std::string local;
  auto &original = source ? *source : local;
  const auto result = read_once(path, root, &original);
  if (result == ReadResult::Ready || result == ReadResult::Error)
    return result == ReadResult::Ready;
  original = serialize_config(*root, original);
  if (result == ReadResult::Rewrite)
    return write_text_atomic(path, original);
  if (path == ksud::YUKIZYGISK_CONFIG_PATH) {
    if (!withdraw_fixed_policy())
      return false;
  } else if (result == ReadResult::Missing) {
    return true;
  }
  return write_text_atomic(path, original);
}

} // namespace detail

inline bool read(const std::string &path, json::Value *root,
                 std::string *source = nullptr) {
  struct stat pending{};
  if (lstat((path + detail::kPolicyPending).c_str(), &pending) == 0) {
    detail::ScopedFd lock(detail::lock_config(path));
    return lock.get() >= 0 && detail::read_locked(path, root, source);
  }
  if (errno != ENOENT)
    return false;
  const auto result = detail::read_once(path, root, source);
  if (result == detail::ReadResult::Ready)
    return true;
  if (result == detail::ReadResult::Error && errno != EAGAIN)
    return false;
  if (result == detail::ReadResult::Missing &&
      path != ksud::YUKIZYGISK_CONFIG_PATH)
    return true;
  detail::ScopedFd lock(detail::lock_config(path));
  return lock.get() >= 0 && detail::read_locked(path, root, source);
}

inline bool protection_enabled(const std::string &path) {
  json::Value root;
  return read(path, &root) && root.at("crash_protection").bool_or(false);
}

inline bool update(const std::string &path,
                   const std::vector<std::string> &pairs,
                   bool (*stage_policy)(const json::Value &) = nullptr,
                   bool (*apply_runtime)(bool pending) = nullptr) {
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
  std::string original;
  if (!detail::read_locked(path, &root, &original))
    return false;
  // A nonempty lock file records a reload until a writer has notified the
  // daemon.
  const off_t marker = lseek(lock.get(), 0, SEEK_END);
  if (marker < 0)
    return false;
  // An explicit runtime-only set also synchronizes the running daemon.
  bool pending = marker != 0 || changes.at("fixed_sepolicy").is_null();
  for (const auto &[key, value] : changes.as_object()) {
    if (key != "fixed_sepolicy" &&
        json::dump(root.at(key)) != json::dump(value))
      pending = true;
    root[key] = value;
  }
  bool boot_matches = true;
  if (stage_policy && !detail::boot_matches_config(root, &boot_matches))
    return false;
  if (stage_policy && (!boot_matches || changes.contains("fixed_sepolicy")) &&
      !detail::require_private_policy_config(path))
    return false;
  if (pending && !marker &&
      (ftruncate(lock.get(), 1) != 0 || fsync(lock.get()) != 0))
    return false;
  const auto candidate = detail::serialize_config(root, original, &changes);
  if (!boot_matches) {
    // /data and /metadata can be different filesystems. An interrupted writer
    // leaves a candidate that readers resolve against the effective boot file.
    if (candidate.size() > crash::kMaxDocument) {
      errno = EFBIG;
      return false;
    }
    auto journal = candidate;
    journal.append(2, '\0').append(original);
    if (!detail::write_text_atomic(path + detail::kPolicyPending, journal,
                                   detail::kMaxPolicyJournal))
      return false;
    if (!stage_policy(root)) {
      const int error = errno;
      (void)detail::recover_policy_config(path);
      errno = error;
      return false;
    }
    if (!detail::recover_policy_config(path, &candidate))
      return false;
  } else if ((stage_policy && !stage_policy(root)) ||
             !detail::write_text_atomic(path, candidate)) {
    return false;
  }
  if (!apply_runtime)
    return true;
  if (!apply_runtime(pending))
    return false;
  return !pending || (ftruncate(lock.get(), 0) == 0 && fsync(lock.get()) == 0);
}

} // namespace yukizygisk::settings
