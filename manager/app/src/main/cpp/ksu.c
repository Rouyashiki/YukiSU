//
// Created by weishu on 2022/12/9.
//

#include <android/log.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ksu.h"
#include "prelude.h"
#include <errno.h>

static int fd = -1;

static inline int scan_driver_fd() {
  const char *kName = "[ksu_driver]";
  DIR *fd_dir = opendir("/proc/self/fd");
  if (!fd_dir) {
    return -1;
  }

  int found = -1;
  struct dirent *de;
  char path[64];
  char target[PATH_MAX];

  while ((de = readdir(fd_dir)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }

    char *endptr = nullptr;
    long fd_long = strtol(de->d_name, &endptr, 10);
    if (!de->d_name[0] || *endptr != '\0' || fd_long < 0 || fd_long > INT_MAX) {
      continue;
    }

    int path_length =
        snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
      continue;
    }
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) {
      continue;
    }
    target[n] = '\0';

    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;

    if (strstr(base, kName)) {
      found = (int)fd_long;
      break;
    }
  }

  closedir(fd_dir);
  return found;
}

static inline int request_driver_fd_via_prctl() {
  struct ksu_prctl_get_fd_cmd cmd = {
      .result = -1,
      .fd = -1,
  };

  /*
   * Ask kernel for a dedicated KSU driver fd when none is inherited.
   * This is required for manager coexistence, where one manager process
   * may not have an existing [ksu_driver] fd to scan from /proc/self/fd.
   */
  long ret = prctl(KSU_PRCTL_GET_FD, &cmd, 0, 0, 0);
  (void)ret;
  if (cmd.result == 0 && cmd.fd >= 0) {
    return cmd.fd;
  }
  return -1;
}

static int ksuctl(unsigned long op, void *arg) {
  if (fd < 0) {
    fd = scan_driver_fd();
    if (fd < 0) {
      fd = request_driver_fd_via_prctl();
    }
  }
  return ioctl(fd, op, arg);
}

int ksu_kasumi_ioctl(unsigned long op, void *arg) { return ksuctl(op, arg); }

int ksu_kasumi_open_fd(void) {
  int driver_fd = -1;
  (void)syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0,
                &driver_fd);
  return driver_fd;
}

int ksu_grant_root(void) { return ksuctl(KSU_IOCTL_GRANT_ROOT, NULL); }

static struct ksu_get_info_cmd g_version = {0};

// Reset cached info (call after SuperKey authentication)
void reset_cached_info() { memset(&g_version, 0, sizeof(g_version)); }

static int query_info(struct ksu_get_info_cmd *info) {
  int result = ksuctl(KSU_IOCTL_GET_INFO, info);
  if (result < 0 && errno == ENOTTY) {
    result = ksuctl(KSU_IOCTL_GET_INFO_LEGACY, info);
    if (result == 0)
      ksuctl(KSU_IOCTL_GET_UAPI_VERSION, &info->uapi_version);
  }
  return result;
}

struct ksu_get_info_cmd get_info() {
  if (!g_version.version)
    query_info(&g_version);
  return g_version;
}

uint32_t get_version() {
  auto info = get_info();
  return info.version;
}

uint32_t get_uapi_version() {
  struct ksu_get_info_cmd info = {};
  query_info(&info);
  return info.uapi_version;
}

// UAPI contract version this manager was built against. Compared against the
// kernel's get_uapi_version() to detect manager/kernel skew.
uint32_t get_manager_uapi_version() { return (uint32_t)KERNEL_SU_UAPI_VERSION; }

bool get_allow_list(struct ksu_get_allow_list_cmd *cmd) {
  return ksuctl(KSU_IOCTL_GET_ALLOW_LIST, cmd) == 0;
}

int get_superuser_count(void) {
  struct ksu_new_get_allow_list_cmd cmd = {
      .count = 0,
  };
  if (ksuctl(KSU_IOCTL_NEW_GET_ALLOW_LIST, &cmd) == 0) {
    return (int)cmd.total_count;
  }
  // fallback: use old API and return count
  struct ksu_get_allow_list_cmd old_cmd = {};
  if (get_allow_list(&old_cmd)) {
    return (int)old_cmd.count;
  }
  return 0;
}

bool is_safe_mode() {
  struct ksu_check_safemode_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_CHECK_SAFEMODE, &cmd) == 0) {
    return cmd.in_safe_mode;
  }
  return false;
}

bool is_manager() {
  // Do a fresh query to avoid stale cached flags after kernel crowns manager.
  struct ksu_get_info_cmd info = {};
  if (query_info(&info) == 0 && info.version > 0) {
    g_version = info;
    return (info.flags & KSU_GET_INFO_FLAG_MANAGER) != 0;
  }
  return false;
}

bool is_lkm_bundled() {
  auto info = get_info();
  return (info.flags & KSU_GET_INFO_FLAG_LKM) != 0 &&
         (info.flags & KSU_GET_INFO_FLAG_BUNDLED) != 0;
}

bool is_late_load_mode() {
  auto info = get_info();
  if (info.version > 0) {
    return (info.flags & KSU_GET_INFO_FLAG_LATE_LOAD) != 0;
  }
  return false;
}

bool is_image_patch_mode() {
  struct ksu_get_load_mode_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_GET_LOAD_MODE, &cmd) != 0) {
    return false;
  }
  return cmd.mode == KSU_LOAD_MODE_IMAGE_PATCH;
}

int get_load_mode() {
  struct ksu_get_load_mode_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_GET_LOAD_MODE, &cmd) != 0) {
    return 0;
  }
  return (int)cmd.mode;
}

bool uid_should_umount(int uid) {
  struct ksu_uid_should_umount_cmd cmd = {};
  cmd.uid = uid;
  if (ksuctl(KSU_IOCTL_UID_SHOULD_UMOUNT, &cmd) == 0) {
    return cmd.should_umount;
  }
  return false;
}

uint32_t get_dynamic_managers(struct ksu_dynamic_manager_app *apps,
                              uint32_t max_count) {
  struct ksu_get_dynamic_managers_cmd cmd = {};
  cmd.count = max_count;
  cmd.apps = (uint64_t)(uintptr_t)apps;
  if (ksuctl(KSU_IOCTL_GET_DYNAMIC_MANAGERS, &cmd) == 0) {
    return cmd.count;
  }
  return 0;
}

bool set_app_profile(const struct app_profile *profile) {
  struct ksu_set_app_profile_cmd cmd = {};
  cmd.profile = *profile;
  return ksuctl(KSU_IOCTL_SET_APP_PROFILE, &cmd) == 0;
}

int get_app_profile(struct app_profile *profile) {
  struct ksu_get_app_profile_cmd cmd = {.profile = *profile};
  if (ksuctl(KSU_IOCTL_GET_APP_PROFILE, &cmd) == 0) {
    *profile = cmd.profile;
    return 0;
  }
  return -1;
}

bool set_su_enabled(bool enabled) {
  struct ksu_set_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_SU_COMPAT;
  cmd.value = enabled ? 1 : 0;
  return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

bool is_su_enabled() {
  struct ksu_get_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_SU_COMPAT;
  if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) == 0 && cmd.supported) {
    return cmd.value != 0;
  }
  return false;
}

bool set_magisk_compat_enabled(bool enabled) {
  struct ksu_set_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_MAGISK_COMPAT;
  cmd.value = enabled ? 1 : 0;
  return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

bool is_magisk_compat_enabled() {
  struct ksu_get_feature_cmd cmd = {};
  cmd.feature_id = KSU_FEATURE_MAGISK_COMPAT;
  if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) == 0 && cmd.supported) {
    return cmd.value != 0;
  }
  return false;
}

int su_prompt_ready(uint64_t request_id, uint64_t nonce) {
  struct ksu_su_prompt_key key = {
      .request_id = request_id,
      .nonce = nonce,
  };
  int ret = ksuctl(KSU_IOCTL_SU_PROMPT_READY, &key);
  if (ret >= 0)
    return ret;
  return errno == ENOTTY ? 12000 : -errno;
}

bool submit_su_prompt(uint64_t request_id, uint64_t nonce, uint32_t choice,
                      const char *package_name) {
  struct ksu_su_prompt_verdict verdict = {
      .request_id = request_id,
      .nonce = nonce,
      .choice = choice,
  };

  if (package_name) {
    strncpy(verdict.package, package_name, sizeof(verdict.package) - 1);
    verdict.package[sizeof(verdict.package) - 1] = '\0';
  }
  return ksuctl(KSU_IOCTL_SUBMIT_SU_PROMPT, &verdict) == 0;
}

static inline bool get_feature(uint32_t feature_id, uint64_t *out_value,
                               bool *out_supported) {
  struct ksu_get_feature_cmd cmd = {};
  cmd.feature_id = feature_id;
  if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) != 0) {
    return false;
  }
  if (out_value)
    *out_value = cmd.value;
  if (out_supported)
    *out_supported = (cmd.supported & KSU_FEATURE_FLAG_SUPPORTED) != 0;
  return true;
}

static inline bool set_feature(uint32_t feature_id, uint64_t value) {
  struct ksu_set_feature_cmd cmd = {};
  cmd.feature_id = feature_id;
  cmd.value = value;
  return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

uint8_t query_feature_flags(uint32_t feature_id) {
  struct ksu_get_feature_cmd cmd = {.feature_id = feature_id};
  return ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) == 0 ? cmd.supported : 0;
}

int64_t query_feature(uint32_t feature_id) {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(feature_id, &value, &supported) || !supported) {
    return -1;
  }
  // Feature values are flags today, so the truncation an INT64_MAX-and-above
  // value would suffer is theoretical. Clamp rather than wrap into a negative,
  // which the caller reads as "unsupported".
  if (value > (uint64_t)INT64_MAX) {
    return INT64_MAX;
  }
  return (int64_t)value;
}

bool set_kernel_umount_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_KERNEL_UMOUNT, enabled ? 1 : 0);
}

bool is_kernel_umount_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_KERNEL_UMOUNT, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_sulog_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_SULOG, enabled ? 1 : 0);
}

bool is_sulog_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_SULOG, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_adb_root_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_ADB_ROOT, enabled ? 1 : 0);
}

bool is_adb_root_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_ADB_ROOT, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_selinux_hide_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_SELINUX_HIDE, enabled ? 1 : 0);
}

bool is_selinux_hide_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_SELINUX_HIDE, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_default_no_new_privs_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_DEFAULT_NO_NEW_PRIVS, enabled ? 1 : 0);
}

bool is_default_no_new_privs_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_DEFAULT_NO_NEW_PRIVS, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

bool set_hide_bootloader_enabled(bool enabled) {
  return set_feature(KSU_FEATURE_HIDE_BOOTLOADER, enabled ? 1 : 0);
}

bool is_hide_bootloader_enabled() {
  uint64_t value = 0;
  bool supported = false;
  if (!get_feature(KSU_FEATURE_HIDE_BOOTLOADER, &value, &supported)) {
    return false;
  }
  if (!supported) {
    return false;
  }
  return value != 0;
}

void get_full_version(char *buff) {
  struct ksu_get_full_version_cmd cmd = {0};
  if (ksuctl(KSU_IOCTL_GET_FULL_VERSION, &cmd) == 0) {
    strncpy(buff, cmd.version_full, KSU_FULL_VERSION_STRING - 1);
    buff[KSU_FULL_VERSION_STRING - 1] = '\0';
  } else {
    buff[0] = '\0';
  }
}

void get_hook_type(char *buff) {
  struct ksu_hook_type_cmd cmd = {0};
  if (ksuctl(KSU_IOCTL_HOOK_TYPE, &cmd) == 0) {
    strncpy(buff, cmd.hook_type, 32 - 1);
    buff[32 - 1] = '\0';
  } else {
    buff[0] = '\0';
  }
}

// SuperKey authentication using prctl syscall (SECCOMP-safe)
bool authenticate_superkey(const char *superkey) {
  if (!superkey) {
    LogDebug("authenticate_superkey: superkey is null");
    return false;
  }

  // Method 1: Use prctl (SECCOMP-safe, recommended)
  struct ksu_superkey_prctl_cmd prctl_cmd = {};
  strncpy(prctl_cmd.superkey, superkey, sizeof(prctl_cmd.superkey) - 1);
  prctl_cmd.superkey[sizeof(prctl_cmd.superkey) - 1] = '\0';
  prctl_cmd.timestamp = (uint64_t)time(nullptr);
  prctl_cmd.result = -1; // Initialize with error
  prctl_cmd.fd = -1;

  LogDebug("authenticate_superkey: trying prctl method...");

  // Use prctl syscall with SuperKey magic
  // prctl(KSU_PRCTL_SUPERKEY_AUTH, &cmd_struct, 0, 0, 0)
  long ret = prctl(KSU_PRCTL_SUPERKEY_AUTH, &prctl_cmd, 0, 0, 0);
  (void)ret;

  // Give task_work more time to execute
  // task_work runs asynchronously, need to wait for completion
  usleep(50000); // 50ms

  LogDebug("authenticate_superkey: prctl ret=%ld, cmd.result=%d, cmd.fd=%d",
           ret, prctl_cmd.result, prctl_cmd.fd);

  if (prctl_cmd.result == 0 && prctl_cmd.fd >= 0) {
    // Authentication successful via prctl
    fd = prctl_cmd.fd;
    reset_cached_info(); // Clear cached version/flags so next is_manager()
                         // check is fresh

    // Verify the fd is working AND that we are now manager
    // Retry up to 5 times with increasing delays to ensure kernel state is
    // ready
    for (int retry = 0; retry < 5; retry++) {
      struct ksu_get_info_legacy_cmd verify_cmd = {};
      if (ioctl(fd, KSU_IOCTL_GET_INFO_LEGACY, &verify_cmd) == 0 &&
          verify_cmd.version > 0) {
        // Check if is_manager flag is set (0x2)
        if (verify_cmd.flags & 0x2) {
          LogDebug("authenticate_superkey: prctl success, fd=%d, version=%d, "
                   "flags=0x%x, retry=%d",
                   fd, verify_cmd.version, verify_cmd.flags, retry);
          return true;
        }
        LogDebug("authenticate_superkey: fd ok but not manager yet, "
                 "flags=0x%x, retry=%d",
                 verify_cmd.flags, retry);
      } else {
        LogDebug("authenticate_superkey: ioctl failed, retry=%d", retry);
      }
      // Exponential backoff: 20ms, 40ms, 80ms, 160ms, 320ms
      usleep(20000 << retry);
    }
    // Final check
    struct ksu_get_info_legacy_cmd final_cmd = {};
    if (ioctl(fd, KSU_IOCTL_GET_INFO_LEGACY, &final_cmd) == 0 &&
        final_cmd.version > 0 && (final_cmd.flags & 0x2)) {
      LogDebug("authenticate_superkey: success after retries, fd=%d", fd);
      return true;
    }
    LogDebug("authenticate_superkey: failed to become manager after retries");
  }

  // Method 2: Fallback to reboot syscall (only if we already have fd)
  // Reboot syscall is blocked by SECCOMP for non-manager apps, so skip if no fd
  if (fd >= 0) {
    LogDebug("authenticate_superkey: prctl failed, trying reboot method (have "
             "fd)...");
    struct ksu_superkey_reboot_cmd reboot_cmd = {};
    strncpy(reboot_cmd.superkey, superkey, sizeof(reboot_cmd.superkey) - 1);
    reboot_cmd.superkey[sizeof(reboot_cmd.superkey) - 1] = '\0';
    reboot_cmd.result = -1; // Initialize with error
    reboot_cmd.fd = -1;

    // Use reboot syscall with SuperKey magic
    // reboot(KSU_INSTALL_MAGIC1, KSU_SUPERKEY_MAGIC2, 0, &cmd)
    long ret = syscall(__NR_reboot, KSU_INSTALL_MAGIC1, KSU_SUPERKEY_MAGIC2, 0,
                       &reboot_cmd);
    (void)ret;

    // Give task_work a chance to execute
    usleep(10000); // 10ms

    LogDebug("authenticate_superkey: reboot ret=%ld, cmd.result=%d, cmd.fd=%d",
             ret, reboot_cmd.result, reboot_cmd.fd);

    if (reboot_cmd.result == 0 && reboot_cmd.fd >= 0) {
      // Authentication successful via reboot
      fd = reboot_cmd.fd;
      reset_cached_info(); // Clear cached version/flags so next is_manager()
                           // check is fresh
      // Verify is_manager flag with retries
      for (int retry = 0; retry < 5; retry++) {
        struct ksu_get_info_legacy_cmd verify_cmd = {};
        if (ioctl(fd, KSU_IOCTL_GET_INFO_LEGACY, &verify_cmd) == 0 &&
            verify_cmd.version > 0 && (verify_cmd.flags & 0x2)) {
          LogDebug("authenticate_superkey: reboot success, fd=%d, flags=0x%x",
                   fd, verify_cmd.flags);
          return true;
        }
        usleep(20000 << retry);
      }
      LogDebug("authenticate_superkey: reboot fd ok but not manager");
    }
  } else {
    LogDebug("authenticate_superkey: skipping reboot method (no fd, would "
             "crash due to SECCOMP)");
  }

  // Method 3: Fallback - try ioctl if we already have an fd
  if (fd >= 0) {
    struct ksu_superkey_auth_cmd ioctl_cmd = {};
    strncpy(ioctl_cmd.superkey, superkey, sizeof(ioctl_cmd.superkey) - 1);
    ioctl_cmd.superkey[sizeof(ioctl_cmd.superkey) - 1] = '\0';
    ioctl_cmd.result = 0xFFFFFFFF;

    if (ksuctl(KSU_IOCTL_SUPERKEY_AUTH, &ioctl_cmd) == 0) {
      LogDebug("authenticate_superkey: ioctl success, result=%u",
               ioctl_cmd.result);
      if (ioctl_cmd.result == 0) {
        reset_cached_info(); // Clear cached info on success
        // Verify is_manager flag with retries
        for (int retry = 0; retry < 5; retry++) {
          struct ksu_get_info_legacy_cmd verify_cmd = {};
          if (ioctl(fd, KSU_IOCTL_GET_INFO_LEGACY, &verify_cmd) == 0 &&
              verify_cmd.version > 0 && (verify_cmd.flags & 0x2)) {
            LogDebug("authenticate_superkey: ioctl auth success, flags=0x%x",
                     verify_cmd.flags);
            return true;
          }
          usleep(20000 << retry);
        }
        LogDebug("authenticate_superkey: ioctl auth ok but not manager");
      }
    }
  }

  LogDebug("authenticate_superkey: all methods failed (kernel may not have "
           "prctl hook enabled)");
  return false;
}

// Check if KSU driver is present (without authentication)
// This checks if the driver fd can be found
bool ksu_driver_present(void) {
  if (fd >= 0) {
    return true;
  }
  fd = scan_driver_fd();
  return fd >= 0;
}

// Check if SuperKey is configured in kernel
bool is_superkey_configured(void) {
  // First try new ioctl
  struct ksu_superkey_status_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_SUPERKEY_STATUS, &cmd) == 0) {
    LogDebug("is_superkey_configured: ioctl success, is_configured=%d",
             cmd.enabled);
    return cmd.enabled != 0;
  }

  // If ioctl failed, kernel probably doesn't have SuperKey support
  LogDebug("is_superkey_configured: ioctl failed, assuming not configured");
  return false;
}

// Check if already authenticated via SuperKey
bool is_superkey_authenticated(void) {
  struct ksu_superkey_status_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_SUPERKEY_STATUS, &cmd) == 0) {
    LogDebug("is_superkey_authenticated: ioctl success, is_authenticated=%d",
             cmd.authenticated);
    return cmd.authenticated != 0;
  }

  LogDebug("is_superkey_authenticated: ioctl failed");
  return false;
}

// Check whether manager signature is considered OK (from kernel's view)
bool is_signature_ok(void) {
  struct ksu_superkey_status_cmd cmd = {};
  if (ksuctl(KSU_IOCTL_SUPERKEY_STATUS, &cmd) == 0) {
    LogDebug("is_signature_ok: ioctl success, signature_ok=%d",
             cmd.signature_ok);
    return cmd.signature_ok != 0;
  }

  LogDebug("is_signature_ok: ioctl failed");
  return false;
}
