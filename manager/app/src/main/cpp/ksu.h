//
// Created by weishu on 2022/12/9.
//

#ifndef KERNELSU_KSU_H
#define KERNELSU_KSU_H

#include "prelude.h"
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

// --- Kernel UAPI headers (single source of truth) ---
// These provide: ksu_feature_id, app_profile, root_profile,
// ksu_get_info_cmd, ksu_report_event_cmd, ksu_check_safemode_cmd,
// ksu_get_feature_cmd, ksu_set_feature_cmd, ksu_get_allow_list_cmd,
// ksu_new_get_allow_list_cmd, ksu_uid_granted_root_cmd,
// ksu_uid_should_umount_cmd, ksu_get_manager_appid_cmd,
// ksu_get_manager_uid_cmd, ksu_get_app_profile_cmd,
// ksu_set_app_profile_cmd, ksu_become_daemon_cmd,
// KSU_IOCTL_*, KSU_INSTALL_MAGIC*, KSU_SUPERKEY_MAGIC2,
// KSU_PRCTL_*, ksu_superkey_prctl_cmd, ksu_superkey_reboot_cmd,
// ksu_prctl_get_fd_cmd
#include "uapi/app_profile.h"
#include "uapi/feature.h"
#include "uapi/supercall.h"

#define KSU_FULL_VERSION_STRING 255

// --- Manager JNI helper declarations ---

uint32_t get_version();
uint32_t get_uapi_version();
uint32_t get_manager_uapi_version();
int ksu_grant_root(void);
int ksu_kasumi_ioctl(unsigned long op, void *arg);
/* Returns a new control fd owned by the caller. */
int ksu_kasumi_open_fd(void);

bool uid_should_umount(int uid);
uint32_t get_dynamic_managers(struct ksu_dynamic_manager_app *apps,
                              uint32_t max_count);

bool is_safe_mode();

bool is_manager();
bool is_late_load_mode();
bool is_lkm_bundled();
bool is_image_patch_mode();
int get_load_mode();

void get_full_version(char *buff);

bool set_app_profile(const struct app_profile *profile);

int get_app_profile(struct app_profile *profile);

void get_hook_type(char *buff);

// Su compat
bool set_su_enabled(bool enabled);
bool is_su_enabled();

bool set_magisk_compat_enabled(bool enabled);
bool is_magisk_compat_enabled();
bool submit_su_prompt(uint64_t request_id, uint64_t nonce, uint32_t choice,
                      const char *package_name);
int su_prompt_ready(uint64_t request_id, uint64_t nonce);

// Kernel umount
bool set_kernel_umount_enabled(bool enabled);
bool is_kernel_umount_enabled();

// Su log
bool set_sulog_enabled(bool enabled);
bool is_sulog_enabled();

// ADB root
bool set_adb_root_enabled(bool enabled);
bool is_adb_root_enabled();

// SELinux hide
bool set_selinux_hide_enabled(bool enabled);
bool is_selinux_hide_enabled();

bool set_default_no_new_privs_enabled(bool enabled);
bool is_default_no_new_privs_enabled();

// Bootloader property hiding
bool set_hide_bootloader_enabled(bool enabled);
bool is_hide_bootloader_enabled();

// Generic feature read for the features that have no dedicated accessor above.
// Returns the feature's value, or -1 when the ioctl fails or the kernel reports
// the feature unsupported -- the caller cannot tell those apart, and neither
// case means "off". Feature ids come from uapi/feature.h.
uint8_t query_feature_flags(uint32_t feature_id);
int64_t query_feature(uint32_t feature_id);

// ksu_get_full_version_cmd, ksu_hook_type_cmd, ksu_superkey_auth_cmd,
// ksu_superkey_status_cmd, KSU_IOCTL_GET_FULL_VERSION, KSU_IOCTL_HOOK_TYPE,
// KSU_IOCTL_SUPERKEY_AUTH, KSU_IOCTL_SUPERKEY_STATUS are all provided by
// uapi/supercall.h included above.

// SuperKey authentication function (uses reboot syscall)
bool authenticate_superkey(const char *superkey);

// Check if SuperKey is configured in kernel
bool is_superkey_configured(void);

// Check if already authenticated via SuperKey
bool is_superkey_authenticated(void);

// Check whether manager signature is considered OK (from kernel's view)
bool is_signature_ok(void);

// Check if KSU driver is present (without authentication)
bool ksu_driver_present(void);

bool get_allow_list(struct ksu_get_allow_list_cmd *);

// Returns total allow-list count only (no full list fetch). Uses new allowlist
// API.
int get_superuser_count(void);

#endif // #ifndef KERNELSU_KSU_H
