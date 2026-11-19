#ifndef YUKIZYGISK_FIXED_POLICY_H
#define YUKIZYGISK_FIXED_POLICY_H

#include "uapi/fixed_policy.h"

struct file;
struct cred;

void yz_fixed_policy_latch(void);
int yz_fixed_policy_boot(bool explicit_enable);
bool yz_fixed_policy_active(void);
int yz_fixed_policy_live_error(struct yz_fixed_policy_status *detail);
void yz_fixed_set_missing(struct yz_fixed_policy_status *status,
			  const char *source, const char *target,
			  const char *class_name, u32 av);
void yz_fixed_policy_missing(const char *source, const char *target,
			     const char *class_name, u32 av);
void yz_fixed_policy_status(struct yz_fixed_policy_status *status);
int yz_fixed_policy_apply(const struct yz_fixed_policy_config *config,
			  struct yz_fixed_policy_status *detail);
int yz_fixed_policy_check(struct file *file, const struct cred *cred,
			  bool receive_only);
int yz_fixed_policy_check_execmem(const struct cred *cred);
int yz_fixed_policy_label(struct file *file, const struct cred *receiver);

#endif
