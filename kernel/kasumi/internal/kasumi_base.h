#ifndef _KASUMI_BASE_H
#define _KASUMI_BASE_H

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/version.h>

#include "uapi/kasumi.h"

#include "klog.h" // IWYU pragma: keep

#if defined(__clang__)
#if __clang_major__ >= 17
#define KASUMI_NOCFI __attribute__((no_sanitize("cfi", "kcfi")))
#else
#define KASUMI_NOCFI __attribute__((no_sanitize("cfi")))
#endif
#else
#define KASUMI_NOCFI
#endif

#define KASUMI_HASH_BITS 12
#define KASUMI_BLOOM_BITS 10
#define KASUMI_BLOOM_SIZE (1 << KASUMI_BLOOM_BITS)
#define KASUMI_BLOOM_MASK (KASUMI_BLOOM_SIZE - 1)
#define KASUMI_MERGE_HASH_BITS 6
#define KASUMI_MERGE_HASH_SIZE (1 << KASUMI_MERGE_HASH_BITS)

#define KASUMI_PATH_BUF 512
#define KASUMI_ITERATE_PATH_BUF 512

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0))
#define KASUMI_FILLDIR_RET_TYPE int
#define KASUMI_FILLDIR_CONTINUE 0
#define KASUMI_FILLDIR_STOP 1
#else
#define KASUMI_FILLDIR_RET_TYPE bool
#define KASUMI_FILLDIR_CONTINUE true
#define KASUMI_FILLDIR_STOP false
#endif

#define KASUMI_UID_ALLOW_MARKER ((void *)1)
#define KASUMI_MAX_MERGE_TARGETS 4

extern bool kasumi_debug_enabled;
bool kasumi_config_debug(void);

#define kasumi_log(fmt, ...)                                                   \
	(void)(kasumi_config_debug() && pr_info("kasumi: " fmt, ##__VA_ARGS__))

#endif /* _KASUMI_BASE_H */
