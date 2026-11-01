#ifndef _KASUMI_FAKE_SELINUXFS_ACCESS_H
#define _KASUMI_FAKE_SELINUXFS_ACCESS_H

#include <linux/types.h>

int kasumi_fake_selinuxfs_access_init(void);
void kasumi_fake_selinuxfs_access_stop_new(void);
void kasumi_fake_selinuxfs_access_exit(void);
bool kasumi_fake_selinuxfs_access_active(void);
bool kasumi_fake_selinuxfs_proc_attr_active(void);
unsigned int kasumi_fake_selinuxfs_delay_ns(void);
u64 kasumi_fake_selinuxfs_spin_count(void);

#endif /* _KASUMI_FAKE_SELINUXFS_ACCESS_H */
