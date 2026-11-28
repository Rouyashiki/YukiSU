#ifndef __KSU_H_AVC_COMPAT
#define __KSU_H_AVC_COMPAT

#include <linux/version.h>
#include "avc.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#define ksu_avc_has_perm_compat(...) avc_has_perm(__VA_ARGS__)
#else
#define ksu_avc_has_perm_compat(...) avc_has_perm(&selinux_state, __VA_ARGS__)
#endif

#endif
