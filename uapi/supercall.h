#ifndef __KSU_UAPI_SUPERCALL_H
#define __KSU_UAPI_SUPERCALL_H

#ifdef __cplusplus
extern "C" {
#endif // #ifdef __cplusplus

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
// __u8/__u16/__u32/__u64/__s32 are provided by <sys/ioctl.h>
// (-> linux/ioctl.h -> asm/types.h) on Linux/Android.
#ifndef __aligned_u64
#define __aligned_u64 __u64 __attribute__((aligned(8)))
#endif // #ifndef __aligned_u64
#endif // #ifdef __KERNEL__

#include "uapi/app_profile.h"
#include "uapi/selinux.h"
#include "uapi/su_path.h"
#include "uapi/uts_view.h"

// Magic numbers for reboot hook
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE
#define KSU_SUPERKEY_MAGIC2 0xCAFE5555

// Magic numbers for prctl hook (SECCOMP-safe)
#define KSU_PRCTL_SUPERKEY_AUTH 0x59554B49 // "YUKI"
#define KSU_PRCTL_GET_FD 0x59554B4A        // "YUKJ"

// prctl command structures
struct ksu_prctl_get_fd_cmd {
  int result;
  int fd;
};

struct ksu_superkey_prctl_cmd {
  char superkey[65];
  __u64 timestamp; // user-supplied Unix epoch seconds; kernel rejects
                   // |now - timestamp| > 30
  int result;
  int fd;
};

struct ksu_superkey_reboot_cmd {
  char superkey[65];
  int result;
  int fd;
};

// IOCTL command structures
struct ksu_become_daemon_cmd {
  __u8 token[65];
};

#define EVENT_POST_FS_DATA 1
#define EVENT_BOOT_COMPLETED 2
#define EVENT_MODULE_MOUNTED 3
#define EVENT_SERVICES 4

/*
 * UAPI contract version. Queried via the dedicated KSU_IOCTL_GET_UAPI_VERSION
 * ioctl (NOT folded into ksu_get_info_cmd) so that GET_INFO stays byte- and
 * number-stable across versions -- the manager's auth/fd handshake depends on
 * GET_INFO working even when kernel and userspace are skewed during an update.
 */
// 3: scoped su-session driver fd
// The su prompt request/verdict ioctls are additive and feature-gated.
// 4: bundled LKM provenance and UAPI-based version matching.
// 5: service stage start/skip result.
#define KERNEL_SU_UAPI_VERSION 5

#define KSU_GET_INFO_FLAG_LKM (1U << 0)
#define KSU_GET_INFO_FLAG_MANAGER (1U << 1)
#define KSU_GET_INFO_FLAG_LATE_LOAD (1U << 2)
#define KSU_GET_INFO_FLAG_PR_BUILD                                             \
  (1U << 3) // reserved (no PR-build concept yet)
#define KSU_GET_INFO_FLAG_BUNDLED (1U << 4)

struct ksu_get_info_cmd {
  __u32 version;
  __u32 flags;    // Output: KSU_GET_INFO_FLAG_* bits
  __u32 features; // Output: max feature ID supported
};

#define KSU_LOAD_MODE_RAMDISK 1
#define KSU_LOAD_MODE_IMAGE_PATCH 2
#define KSU_LOAD_MODE_LATE 3

struct ksu_get_load_mode_cmd {
  __u32 mode;
  __u32 flags;
};

struct ksu_report_event_cmd {
  __u32 event;
};

struct ksu_set_sepolicy_cmd {
  __u64 data_len;     /* Input: bytes of serialized command payload */
  __aligned_u64 data; /* Input: pointer to serialized payload */
};

struct ksu_sepolicy_cmd_hdr {
  __u32 cmd;    /* Input: command type, KSU_SEPOLICY_CMD_* */
  __u32 subcmd; /* Input: command subtype */
};

struct ksu_check_safemode_cmd {
  __u8 in_safe_mode;
};

struct ksu_get_allow_list_cmd {
  __u32 uids[128];
  __u32 count;
  __u8 allow;
};

/* New allowlist API: flexible array, separate count/total (for count-only or
 * pagination) */
struct ksu_new_get_allow_list_cmd {
  __u16 count;       /* Input: buffer size in uids; Output: number of uids
                        returned */
  __u16 total_count; /* Output: total number of uids in list */
  __u32 uids[];      /* Output: array of UIDs (flexible array member) */
};

struct ksu_uid_granted_root_cmd {
  __u32 uid;
  __u8 granted;
};

struct ksu_uid_should_umount_cmd {
  __u32 uid;
  __u8 should_umount;
};

struct ksu_get_manager_appid_cmd {
  __u32 appid;
};

struct ksu_get_manager_uid_cmd {
  __u32 uid;
};

struct ksu_get_app_profile_cmd {
  struct app_profile profile;
};

struct ksu_set_app_profile_cmd {
  struct app_profile profile;
};

struct ksu_get_feature_cmd {
  __u32 feature_id;
  __u64 value;
  __u8 supported;
};

struct ksu_set_feature_cmd {
  __u32 feature_id;
  __u64 value;
};

struct ksu_get_wrapper_fd_cmd {
  __u32 fd;
  __u32 flags;
};

struct ksu_get_sulog_fd_cmd {
  __u32 flags;
};

struct ksu_manage_mark_cmd {
  __u32 operation;
  __s32 pid;
  __u32 result;
};

#define KSU_MARK_GET 1
#define KSU_MARK_MARK 2
#define KSU_MARK_UNMARK 3
#define KSU_MARK_REFRESH 4

struct ksu_nuke_ext4_sysfs_cmd {
  __aligned_u64 arg;
};

struct ksu_add_try_umount_cmd {
  __aligned_u64 arg;
  __u32 flags;
  __u8 mode;
};

struct ksu_list_try_umount_cmd {
  __aligned_u64 arg;
  __u32 buf_size;
};

#define KSU_UMOUNT_WIPE 0
#define KSU_UMOUNT_ADD 1
#define KSU_UMOUNT_DEL 2

#ifndef KSU_FULL_VERSION_STRING
#define KSU_FULL_VERSION_STRING 255
#endif // #ifndef KSU_FULL_VERSION_STRING

struct ksu_get_full_version_cmd {
  char version_full[KSU_FULL_VERSION_STRING];
};

struct ksu_hook_type_cmd {
  char hook_type[32];
};

struct ksu_superkey_auth_cmd {
  char superkey[65];
  __s32 result;
};

struct ksu_superkey_status_cmd {
  __u8 enabled;
  __u8 authenticated;
  __u8 signature_ok;
  __u32 manager_uid;
};

#define KSU_DYNAMIC_MANAGER_MAX_SIGNS 64
#define KSU_DYNAMIC_MANAGER_MAX_APPS 64

struct ksu_dynamic_manager_sign {
  __u32 size;
  char hash[65];
};

struct ksu_dynamic_manager_cmd {
  __u32 count;
  __aligned_u64 signs;
};

#define KSU_DYNAMIC_MANAGER_FLAG_PRESET (1U << 0)
#define KSU_DYNAMIC_MANAGER_FLAG_TRUSTED (1U << 1)

struct ksu_dynamic_manager_app {
  __u32 appid;
  __u32 flags;
};

struct ksu_get_dynamic_managers_cmd {
  __u32 count;
  __u32 total_count;
  __aligned_u64 apps;
};

#define KSU_SU_CHOICE_ALLOW_FOREVER 1
#define KSU_SU_CHOICE_ALLOW_ONCE 2
#define KSU_SU_CHOICE_DENY 3
#define KSU_SU_CHOICE_DENY_HIDE 4

/* Root-only, constrained to allowlist or module-umount profiles. */
struct ksu_magisk_persist_cmd {
  __u32 uid;
  __u8 allow;
  char package[KSU_MAX_PACKAGE_NAME];
};

#define KSU_SU_PROMPT_VERSION 1
#define KSU_SU_PROMPT_COMM_LEN 16

struct ksu_su_prompt_request {
  __u32 version;
  __u32 size;
  __aligned_u64 request_id;
  __aligned_u64 nonce;
  __u32 uid;
  __u32 pid;
  __u32 tgid;
  __u32 reserved;
  char comm[KSU_SU_PROMPT_COMM_LEN];
};

struct ksu_su_prompt_verdict {
  __aligned_u64 request_id;
  __aligned_u64 nonce;
  __u32 choice;
  __u32 reserved;
  char package[KSU_MAX_PACKAGE_NAME];
};

struct ksu_get_su_prompt_fd_cmd {
  __u32 flags;
};

struct ksu_su_prompt_key {
  __aligned_u64 request_id;
  __aligned_u64 nonce;
};

// IOCTL definitions
#define KSU_IOCTL_GRANT_ROOT _IOC(_IOC_NONE, 'K', 1, 0)
#define KSU_IOCTL_GET_INFO _IOC(_IOC_READ, 'K', 2, 0)
#define KSU_IOCTL_REPORT_EVENT _IOC(_IOC_WRITE, 'K', 3, 0)
#define KSU_IOCTL_SET_SEPOLICY _IOC(_IOC_READ | _IOC_WRITE, 'K', 4, 0)
#define KSU_IOCTL_CHECK_SAFEMODE _IOC(_IOC_READ, 'K', 5, 0)
#define KSU_IOCTL_GET_ALLOW_LIST _IOC(_IOC_READ | _IOC_WRITE, 'K', 6, 0)
#define KSU_IOCTL_GET_DENY_LIST _IOC(_IOC_READ | _IOC_WRITE, 'K', 7, 0)
#define KSU_IOCTL_NEW_GET_ALLOW_LIST                                           \
  _IOWR('K', 6, struct ksu_new_get_allow_list_cmd)
#define KSU_IOCTL_NEW_GET_DENY_LIST                                            \
  _IOWR('K', 7, struct ksu_new_get_allow_list_cmd)
#define KSU_IOCTL_UID_GRANTED_ROOT _IOC(_IOC_READ | _IOC_WRITE, 'K', 8, 0)
#define KSU_IOCTL_UID_SHOULD_UMOUNT _IOC(_IOC_READ | _IOC_WRITE, 'K', 9, 0)
#define KSU_IOCTL_GET_MANAGER_APPID _IOC(_IOC_READ, 'K', 10, 0)
#define KSU_IOCTL_GET_APP_PROFILE _IOC(_IOC_READ | _IOC_WRITE, 'K', 11, 0)
#define KSU_IOCTL_SET_APP_PROFILE _IOC(_IOC_WRITE, 'K', 12, 0)
#define KSU_IOCTL_GET_FEATURE _IOC(_IOC_READ | _IOC_WRITE, 'K', 13, 0)
#define KSU_IOCTL_SET_FEATURE _IOC(_IOC_WRITE, 'K', 14, 0)
#define KSU_IOCTL_GET_WRAPPER_FD _IOC(_IOC_WRITE, 'K', 15, 0)
#define KSU_IOCTL_MANAGE_MARK _IOC(_IOC_READ | _IOC_WRITE, 'K', 16, 0)
#define KSU_IOCTL_NUKE_EXT4_SYSFS _IOC(_IOC_WRITE, 'K', 17, 0)
#define KSU_IOCTL_ADD_TRY_UMOUNT _IOC(_IOC_WRITE, 'K', 18, 0)
#define KSU_IOCTL_SET_INIT_PGRP _IO('K', 19)
#define KSU_IOCTL_GET_SULOG_FD _IOC(_IOC_WRITE, 'K', 20, 0)
#define KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT _IO('K', 21)
#define KSU_IOCTL_GET_UAPI_VERSION _IOR('K', 22, __u32)
#define KSU_IOCTL_GET_FULL_VERSION _IOC(_IOC_READ, 'K', 100, 0)
#define KSU_IOCTL_HOOK_TYPE _IOC(_IOC_READ, 'K', 101, 0)
#define KSU_IOCTL_LIST_TRY_UMOUNT _IOC(_IOC_READ | _IOC_WRITE, 'K', 200, 0)
#define KSU_IOCTL_GET_MANAGER_UID _IOC(_IOC_READ, 'K', 201, 0)

/* YukiSU private ioctl range */
#define KSU_IOCTL_SET_DYNAMIC_MANAGERS _IOC(_IOC_WRITE, 'K', 240, 0)
#define KSU_IOCTL_GET_DYNAMIC_MANAGERS                                         \
  _IOWR('K', 241, struct ksu_get_dynamic_managers_cmd)
#define KSU_IOCTL_MAGISK_PERSIST _IOW('K', 242, struct ksu_magisk_persist_cmd)
#define KSU_IOCTL_GET_UTS_VIEW_CONFIG _IOR('K', 243, struct ksu_uts_view_config)
#define KSU_IOCTL_SET_UTS_VIEW_CONFIG _IOW('K', 244, struct ksu_uts_view_config)
#define KSU_IOCTL_GET_UTS_VIEW_STATUS _IOR('K', 245, struct ksu_uts_view_status)
#define KSU_IOCTL_GET_LOAD_MODE _IOR('K', 246, struct ksu_get_load_mode_cmd)
#define KSU_IOCTL_GET_SU_PROMPT_FD                                             \
  _IOW('K', 247, struct ksu_get_su_prompt_fd_cmd)
#define KSU_IOCTL_SUBMIT_SU_PROMPT _IOW('K', 248, struct ksu_su_prompt_verdict)
/* READY returns the remaining decision budget in milliseconds. */
#define KSU_IOCTL_SU_PROMPT_READY _IOW('K', 249, struct ksu_su_prompt_key)
/* Available only on the prompt consumer fd; never grants permission. */
#define KSU_IOCTL_CANCEL_SU_PROMPT _IOW('K', 250, struct ksu_su_prompt_key)

#define KSU_IOCTL_GET_SU_PATH _IOR('K', 251, struct ksu_su_path_config)
#define KSU_IOCTL_SET_SU_PATH _IOW('K', 252, struct ksu_su_path_config)

#define KSU_IOCTL_SUPERKEY_AUTH _IOC(_IOC_READ | _IOC_WRITE, 'K', 107, 0)
#define KSU_IOCTL_SUPERKEY_STATUS _IOC(_IOC_READ, 'K', 108, 0)

#ifdef __cplusplus
}
#endif // #ifdef __cplusplus

#endif // #ifndef __KSU_UAPI_SUPERCALL_H
