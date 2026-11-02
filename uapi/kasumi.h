#ifndef _KASUMI_UAPI_H
#define _KASUMI_UAPI_H

#ifdef __KERNEL__
#include <linux/bits.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/types.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#endif // #ifdef __KERNEL__

#define KSM_PROTOCOL_VERSION 18

#define KSM_MAX_LEN_PATHNAME 256

/*
 * Kasumi inode marking bits (stored in inode->i_mapping->flags)
 * Using high bits to avoid conflict with kernel AS_* flags and SUSFS bits
 * SUSFS uses bits 33-39, we use 40+
 */
#ifdef __KERNEL__
#define AS_FLAGS_KASUMI_HIDE 40
#define BIT_KASUMI_HIDE BIT(40)
/* Marks a directory as containing hidden entries (for fast filldir skip) */
#define AS_FLAGS_KASUMI_DIR_HAS_HIDDEN 41
#define BIT_KASUMI_DIR_HAS_HIDDEN BIT(41)
/* Marks an inode for kstat spoofing */
#define AS_FLAGS_KASUMI_SPOOF_KSTAT 42
#define BIT_KASUMI_SPOOF_KSTAT BIT(42)
/* Marks a directory as having inject/merge rules (fast path for iterate_dir) */
#define AS_FLAGS_KASUMI_DIR_HAS_INJECT 43
#define BIT_KASUMI_DIR_HAS_INJECT BIT(43)
/* Marks an inode as having shadow inode_operations installed (lookup-time i_op
 * override) */
#define AS_FLAGS_KASUMI_IOP_INSTALLED 44
#define BIT_KASUMI_IOP_INSTALLED BIT(44)
/* Marks a directory inode as having shadow file_operations installed for
 * readdir */
#define AS_FLAGS_KASUMI_FOP_INSTALLED 45
#define BIT_KASUMI_FOP_INSTALLED BIT(45)
#endif // #ifdef __KERNEL__

/* API 18: zero preserves policy; bit 0 explicitly enables global rules.
 * Valid for ADD_RULE, ADD_MERGE_RULE and HIDE_RULE. Other bits are rejected. */
#define KSM_RULE_BYPASS_POLICY (1U << 0)
#define KSM_RULE_FLAGS_MASK KSM_RULE_BYPASS_POLICY

struct kasumi_syscall_arg {
  const char *src;
  const char *target;
  int type;
};

struct kasumi_syscall_list_arg {
  char *buf; // Keep as char* for output buffer
  size_t size;
};

/*
 * kstat spoofing structure - allows full control over stat() results
 * Similar to susfs sus_kstat but with Kasumi conventions
 */
struct kasumi_spoof_kstat {
  unsigned long target_ino; /* Target inode number (after mount/overlay) */
  char target_pathname[KSM_MAX_LEN_PATHNAME]; /* Path to spoof */
  unsigned long spoofed_ino;                  /* Spoofed inode number */
  unsigned long spoofed_dev;                  /* Spoofed device number */
  unsigned int spoofed_nlink;                 /* Spoofed link count */
  long long spoofed_size;                     /* Spoofed file size */
  long spoofed_atime_sec;        /* Spoofed access time (seconds) */
  long spoofed_atime_nsec;       /* Spoofed access time (nanoseconds) */
  long spoofed_mtime_sec;        /* Spoofed modification time (seconds) */
  long spoofed_mtime_nsec;       /* Spoofed modification time (nanoseconds) */
  long spoofed_ctime_sec;        /* Spoofed change time (seconds) */
  long spoofed_ctime_nsec;       /* Spoofed change time (nanoseconds) */
  unsigned long spoofed_blksize; /* Spoofed block size */
  unsigned long long spoofed_blocks; /* Spoofed block count */
  int is_static; /* If true, ino won't change after remount */
  int err;       /* Error code for userspace feedback */
};

/* Feature flags returned by KSM_IOC_GET_FEATURES. */
#define KSM_FEATURE_KSTAT_SPOOF (1 << 0)
/* Bit 1 remains reserved for the removed uname feature. */
/* Bit 2 remains reserved for the removed cmdline feature. */
#define KSM_FEATURE_MERGE_DIR (1 << 5)
#define KSM_FEATURE_MOUNT_HIDE                                                 \
  (1 << 6) /* use shared proc mount snapshots for isolated readers */
#define KSM_FEATURE_MAPS_SPOOF                                                 \
  (1 << 7) /* spoof ino/dev/pathname in /proc/pid/maps (read buffer filter) */
#define KSM_FEATURE_STATFS_SPOOF                                               \
  (1 << 8) /* spoof statfs f_type so direct matches resolved                   \
              (INCONSISTENT_MOUNT) */
#define KSM_FEATURE_FAKE_MOUNTINFO                                             \
  (1 << 9) /* shared donor mount views with normalized propagation IDs */
/* Bounded timing compensation for proc attr/current writes. */
#define KSM_FEATURE_SELINUX_FIX (1 << 10)
#define KSM_FEATURE_FAKE_SELINUXFS KSM_FEATURE_SELINUX_FIX
/* Bit 12 remains reserved for removed mount namespace link projection. */
#define KSM_FEATURE_OVERLAY_XATTR_HIDE (1 << 13)
#define KSM_FEATURE_MANAGED_HIDE (1 << 14)
#define KSM_FEATURE_RULE_BYPASS (1 << 15)
#define KSM_FEATURE_SAFE_RULE_RETIRE (1U << 16)

#define KSM_USER_HIDE_PATH_MAX 4096
#define KSM_USER_HIDE_LIVE 0
#define KSM_USER_HIDE_SUSPENDED 1
#define KSM_USER_HIDE_PENDING 0
#define KSM_USER_HIDE_BINDING 1
#define KSM_USER_HIDE_BOUND 2
#define KSM_USER_HIDE_ERROR 3

/* Protocol 17 extension. QUERY returns the first ID greater than rule_id;
 * rule_id == 0 in the reply marks the end. Paths include a terminating NUL,
 * excluded from path_len. UPSERT accepts KSM_RULE_BYPASS_POLICY in flags; QUERY
 * returns stored flags. Other input commands require flags == 0. Reserved
 * fields must be zero. */
struct kasumi_user_hide_arg {
  __u32 size;
  __u32 flags;
  __aligned_u64 rule_id;
  __aligned_u64 generation;
  __aligned_u64 reserved_id;
  __u32 management;
  __u32 binding;
  __s32 error;
  __u32 path_len;
  __aligned_u64 reserved[2];
  char path[KSM_USER_HIDE_PATH_MAX];
};

/*
 * Maps spoof rule: when a /proc/pid/maps line has (target_ino[, target_dev]),
 * replace ino/dev/pathname with spoofed values. target_dev 0 = match any dev.
 */
struct kasumi_maps_rule {
  unsigned long target_ino;
  unsigned long target_dev; /* 0 = match any device */
  unsigned long spoofed_ino;
  unsigned long spoofed_dev;
  char spoofed_pathname[KSM_MAX_LEN_PATHNAME];
  int err;
};

/*
 * Feature config structs - enable + reserved for future custom rules.
 * mount_hide: path_pattern empty = hide all overlay; non-empty = hide only
 * matching (future). maps_spoof: rules via ADD_MAPS_RULE; struct allows future
 * inline rule. statfs_spoof: path empty = auto spoof; non-empty = custom
 * path->f_type (future).
 */
struct kasumi_mount_hide_arg {
  int enable;
  char path_pattern[KSM_MAX_LEN_PATHNAME]; /* reserved: empty = all overlay */
  int err;
};

struct kasumi_maps_spoof_arg {
  int enable;
  /* reserved for future: inline rule, batch config */
  char reserved[sizeof(struct kasumi_maps_rule)];
  int err;
};

struct kasumi_statfs_spoof_arg {
  int enable;
  char path[KSM_MAX_LEN_PATHNAME]; /* reserved: empty = auto */
  unsigned long spoof_f_type;      /* reserved: 0 = use d_real_inode */
  int err;
};

#define KSM_CONFLICT_MAX 16
#define KSM_CONFLICT_NAME_LEN 64

#define KSM_CONFLICT_LOADED_MODULE 1
#define KSM_CONFLICT_BUILTIN_CONFIG 2
#define KSM_CONFLICT_MODULE_LOAD 3

/* Protocol 18 diagnostics persist for the host module's lifetime. Repeated
 * (kind, target) pairs replace their sequence; a full queue replaces its oldest
 * entry. GET does not acknowledge events or activate VFS. Entries are
 * unordered, target is NUL-terminated, and all reserved fields are zero on
 * output. */
struct kasumi_conflict {
  __aligned_u64 sequence;
  __u32 kind;
  __u32 reserved;
  char target[KSM_CONFLICT_NAME_LEN];
};

struct kasumi_conflicts_arg {
  __u32 count;
  __u32 reserved;
  struct kasumi_conflict events[KSM_CONFLICT_MAX];
};

/* Acknowledge through the maximum sequence in the displayed snapshot. Zero is
 * a no-op; a sequence not yet issued returns -EINVAL. Newer events survive. */
struct kasumi_conflict_ack_arg {
  __aligned_u64 sequence;
};

// ioctl definitions (for fd-based mode)
// Must be after struct definitions
#define KSM_IOC_MAGIC 'S'
#define KSM_IOC_GET_ENABLED _IOR(KSM_IOC_MAGIC, 39, int)
#define KSM_IOC_ADD_RULE _IOW(KSM_IOC_MAGIC, 1, struct kasumi_syscall_arg)
#define KSM_IOC_DEL_RULE _IOW(KSM_IOC_MAGIC, 2, struct kasumi_syscall_arg)
#define KSM_IOC_HIDE_RULE _IOW(KSM_IOC_MAGIC, 3, struct kasumi_syscall_arg)
#define KSM_IOC_CLEAR_ALL _IO(KSM_IOC_MAGIC, 5)
#define KSM_IOC_GET_VERSION _IOR(KSM_IOC_MAGIC, 6, int)
#define KSM_IOC_LIST_RULES                                                     \
  _IOWR(KSM_IOC_MAGIC, 7, struct kasumi_syscall_list_arg)
#define KSM_IOC_SET_DEBUG _IOW(KSM_IOC_MAGIC, 8, int)
#define KSM_IOC_REORDER_MNT_ID _IO(KSM_IOC_MAGIC, 9)
#define KSM_IOC_SET_STEALTH _IOW(KSM_IOC_MAGIC, 10, int)
/* An empty src clears xattr targets when OVERLAY_XATTR_HIDE is advertised. */
#define KSM_IOC_HIDE_OVERLAY_XATTRS                                            \
  _IOW(KSM_IOC_MAGIC, 11, struct kasumi_syscall_arg)
#define KSM_IOC_ADD_MERGE_RULE                                                 \
  _IOW(KSM_IOC_MAGIC, 12, struct kasumi_syscall_arg)
/* ABI-reserved legacy slot; pure virtual kernels return -EOPNOTSUPP. */
#define KSM_IOC_SET_MIRROR_PATH                                                \
  _IOW(KSM_IOC_MAGIC, 14, struct kasumi_syscall_arg)
#define KSM_IOC_ADD_SPOOF_KSTAT                                                \
  _IOW(KSM_IOC_MAGIC, 15, struct kasumi_spoof_kstat)
#define KSM_IOC_UPDATE_SPOOF_KSTAT                                             \
  _IOW(KSM_IOC_MAGIC, 16, struct kasumi_spoof_kstat)
/* Command 18 remains reserved for the removed cmdline operation. */
#define KSM_IOC_GET_FEATURES _IOR(KSM_IOC_MAGIC, 19, int)
#define KSM_IOC_SET_ENABLED _IOW(KSM_IOC_MAGIC, 20, int)
#define KSM_IOC_GET_HOOKS                                                      \
  _IOWR(KSM_IOC_MAGIC, 22, struct kasumi_syscall_list_arg)
#define KSM_IOC_ADD_MAPS_RULE _IOW(KSM_IOC_MAGIC, 23, struct kasumi_maps_rule)
#define KSM_IOC_CLEAR_MAPS_RULES _IO(KSM_IOC_MAGIC, 24)
#define KSM_IOC_SET_MOUNT_HIDE                                                 \
  _IOW(KSM_IOC_MAGIC, 25, struct kasumi_mount_hide_arg)
#define KSM_IOC_SET_MAPS_SPOOF                                                 \
  _IOW(KSM_IOC_MAGIC, 26, struct kasumi_maps_spoof_arg)
#define KSM_IOC_SET_STATFS_SPOOF                                               \
  _IOW(KSM_IOC_MAGIC, 27, struct kasumi_statfs_spoof_arg)
/* Commands 17 and 28 remain reserved for removed uname operations. */
#define KSM_IOC_SELINUX_FIX _IOW(KSM_IOC_MAGIC, 29, int)

/* Command 38 remains reserved for removed mount-hide mode selection. */

#define KSM_IOC_USER_HIDE_UPSERT                                               \
  _IOWR(KSM_IOC_MAGIC, 40, struct kasumi_user_hide_arg)
#define KSM_IOC_USER_HIDE_DELETE                                               \
  _IOWR(KSM_IOC_MAGIC, 41, struct kasumi_user_hide_arg)
#define KSM_IOC_USER_HIDE_QUERY                                                \
  _IOWR(KSM_IOC_MAGIC, 42, struct kasumi_user_hide_arg)
#define KSM_IOC_USER_HIDE_CLEAR _IO(KSM_IOC_MAGIC, 43)
#define KSM_IOC_USER_HIDE_RETRY                                                \
  _IOWR(KSM_IOC_MAGIC, 44, struct kasumi_user_hide_arg)

/* Command 45 is reserved for the standalone instance-watch ABI. */

/* These diagnostics remain available when VFS initialization is rejected. */
#define KSM_IOC_GET_CONFLICTS                                                  \
  _IOR(KSM_IOC_MAGIC, 46, struct kasumi_conflicts_arg)
#define KSM_IOC_ACK_CONFLICTS                                                  \
  _IOW(KSM_IOC_MAGIC, 47, struct kasumi_conflict_ack_arg)

#endif /* _KASUMI_UAPI_H */
