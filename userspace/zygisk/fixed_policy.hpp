#pragma once

#include "crash_evidence.hpp"
#include "uapi/fixed_policy.h"

#include <cstdio>
#include <cstring>
#include <sys/file.h>

namespace yukizygisk::fixed_policy {

inline constexpr char kRebootRequired[] =
    "/dev/ksu-yukizygisk-policy.reboot-required";

inline bool domain_name(const std::string &name) {
  if (name.empty() || name.size() >= YZ_FIXED_NAME_MAX || name == "self")
    return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (i != 0 && c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}

inline bool encode(const json::Value &root, yz_fixed_policy_config *config) {
  *config = {};
  config->version = YZ_FIXED_POLICY_VERSION;
  const auto &enabled = root.at("fixed_sepolicy");
  if (!enabled.is_null() && !enabled.is_bool()) {
    errno = EINVAL;
    return false;
  }
  config->enabled = enabled.bool_or(false);
  return true;
}

inline bool write_boot(const std::string &path, const json::Value &root,
                       bool once = false,
                       bool (*prepare_file)(const std::string &) = nullptr) {
  yz_fixed_policy_config config{};
  if (!encode(root, &config))
    return false;
  const auto separator = path.find_last_of('/');
  const std::string parent = separator == std::string::npos ? "."
                             : separator == 0 ? "/"
                                              : path.substr(0, separator);
  const int directory =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0)
    return false;
  struct stat status{};
  if (fstat(directory, &status) != 0 || status.st_uid != 0 ||
      (status.st_mode & 0022)) {
    close(directory);
    errno = EPERM;
    return false;
  }
  std::string temporary = path + ".tmp.XXXXXX";
  const int fd = mkstemp(temporary.data());
  if (fd < 0) {
    close(directory);
    return false;
  }
  const char *bytes = reinterpret_cast<const char *>(&config);
  size_t written = 0;
  while (written < sizeof(config)) {
    const ssize_t count = write(fd, bytes + written, sizeof(config) - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (count == 0)
        errno = EIO;
      break;
    }
    written += static_cast<size_t>(count);
  }
  bool ok = written == sizeof(config) &&
            (!prepare_file || prepare_file(temporary)) && fsync(fd) == 0;
  int saved = errno;
  if (ok) {
    const int result = once ? link(temporary.c_str(), path.c_str())
                            : rename(temporary.c_str(), path.c_str());
    ok = result == 0 || (once && errno == EEXIST);
    if (ok)
      ok = fsync(directory) == 0;
    if (!ok)
      saved = errno;
  }
  close(fd);
  (void)unlink(temporary.c_str());
  close(directory);
  errno = saved;
  return ok;
}

inline bool reboot_required() { return access(kRebootRequired, F_OK) == 0; }

inline bool mark_reboot_required() {
  const int fd =
      open(kRebootRequired,
           O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  return fd >= 0 ? close(fd) == 0 : errno == EEXIST;
}

// The kernel may have latched the preinit profile before /data was restored.
inline bool record_boot_withdrawal(const yz_fixed_policy_config &previous,
                                   const json::Value &settings) {
  if (!previous.enabled)
    return true;
  yz_fixed_policy_config next{};
  if (!encode(settings, &next))
    return false;
  return next.enabled || mark_reboot_required();
}

inline void print_missing_detail(const yz_fixed_policy_status &status) {
  if (status.source[0] || status.target[0] || status.class_name[0] ||
      status.missing_av)
    printf(" source=%.*s target=%.*s class=%.*s missing_av=0x%x",
           static_cast<int>(sizeof(status.source)), status.source,
           static_cast<int>(sizeof(status.target)), status.target,
           static_cast<int>(sizeof(status.class_name)), status.class_name,
           status.missing_av);
}

inline void print_error(const yz_fixed_policy_status &status) {
  printf("Fixed SELinux policy failed (error %d)", status.error);
  print_missing_detail(status);
  putchar('\n');
}

inline void print_denial(const yz_fixed_policy_status &status) {
  printf("Fixed SELinux policy denied access");
  print_missing_detail(status);
  putchar('\n');
}

inline json::Value status_json(const yz_fixed_policy_status &status,
                               bool supported) {
  auto value = json::Value::object();
  value["supported"] = supported;
  value["reboot_required"] = reboot_required();
  value["initialized"] = supported && status.initialized != 0;
  value["requested"] = supported && status.requested != 0;
  value["active"] = supported && status.active != 0;
  value["error"] = supported ? status.error : -EOPNOTSUPP;
  value["missing_source"] =
      std::string(status.source, strnlen(status.source, sizeof(status.source)));
  value["missing_target"] =
      std::string(status.target, strnlen(status.target, sizeof(status.target)));
  value["missing_class"] = std::string(
      status.class_name, strnlen(status.class_name, sizeof(status.class_name)));
  value["missing_av"] = static_cast<double>(status.missing_av);
  return value;
}

} // namespace yukizygisk::fixed_policy
