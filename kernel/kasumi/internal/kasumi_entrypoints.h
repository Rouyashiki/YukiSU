#ifndef _KASUMI_ENTRYPOINTS_H
#define _KASUMI_ENTRYPOINTS_H

#include <asm/ptrace.h>
#include <linux/fs.h>

#include "kasumi_base.h"
#include "kasumi_types.h"

struct file;
long kasumi_handle_ioctl(const struct file *file, unsigned int cmd, void __user *arg);
void kasumi_transaction_release(const struct file *file);

bool kasumi_policy_current_is_view_target(void);
bool kasumi_policy_current_is_spoof_target(void);

KASUMI_FILLDIR_RET_TYPE kasumi_filldir_filter(struct dir_context *ctx,
					      const char *name, int namlen,
					      loff_t offset, u64 ino,
					      unsigned int d_type);

#endif /* _KASUMI_ENTRYPOINTS_H */
