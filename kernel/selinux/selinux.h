#ifndef __KSU_H_SELINUX
#define __KSU_H_SELINUX

#include "linux/cred.h"
#include "linux/types.h"
#include "linux/version.h"

struct file;
struct cred;

#define KERNEL_SU_DOMAIN "ksu"
#define KSU_LEGACY_SU_DOMAIN "su"
#define KERNEL_SU_FILE "ksu_file"

#define KERNEL_SU_CONTEXT "u:r:" KERNEL_SU_DOMAIN ":s0"
#define KSU_LEGACY_SU_CONTEXT "u:r:" KSU_LEGACY_SU_DOMAIN ":s0"
#define KSU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"
#define ZYGOTE_CONTEXT "u:r:zygote:s0"
#define INIT_CONTEXT "u:r:init:s0"
#define ADBD_CONTEXT "u:r:adbd:s0"

int setup_selinux(const char *, struct cred *);

void setenforce(bool);

bool getenforce(void);

bool is_task_ksu_domain(const struct cred *cred);

bool is_ksu_domain(void);

bool is_zygote(const struct cred *cred);

bool is_init(const struct cred *cred);
bool is_adbd(const struct cred *cred);

void apply_kernelsu_rules(void);
void cache_sid(void);

u32 ksu_get_ksu_file_sid(void);

struct ksu_file_load_policy {
	u32 src_type;
	u32 tgt_type;
	u32 tmpfs_type;
	u32 process_type;
	u16 target_class;
	u16 process_class;
	u16 dir_class;
	u16 reserved;
	u32 added_av;
	u32 tmpfs_added_av;
	u32 process_added_av;
	u32 dir_added_av;
	u32 file_lease_refs;
	u32 tmpfs_lease_refs;
	u32 dir_lease_refs;
	u32 process_lease_refs;
};

int ksu_file_load_policy_allow_current(struct file *file,
				       struct ksu_file_load_policy *state);
int ksu_file_load_policy_allow_cred(struct file *file, const struct cred *cred,
				    struct ksu_file_load_policy *state);
int ksu_file_load_policy_allow_execmem_current(
    struct ksu_file_load_policy *state);
int ksu_file_load_policy_allow_execmem_cred(const struct cred *cred,
					    struct ksu_file_load_policy *state);
int ksu_file_load_policy_restore(const struct ksu_file_load_policy *state);

int handle_sepolicy(void __user *user_data, u64 data_len);

void setup_ksu_cred(void);
void escape_to_root_for_adb_root(void);

struct inode;
struct inode_security_struct;
struct inode_security_struct *ksu_selinux_inode(const struct inode *inode);

#if defined(MODULE) && defined(CONFIG_DEBUG_INFO_BTF)
int __init ksu_selinux_inode_init(void);
#else
static inline int ksu_selinux_inode_init(void)
{
	return 0;
}
#endif

#endif // #ifndef __KSU_H_SELINUX
