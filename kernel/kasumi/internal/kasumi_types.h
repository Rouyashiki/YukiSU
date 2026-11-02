#ifndef _KASUMI_TYPES_H
#define _KASUMI_TYPES_H

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/workqueue.h>

#include "kasumi_base.h"

struct kasumi_entry {
	char *src;
	char *target;
	char *source_canonical;
	/* Pinned data source resolved from target when the rule is installed.
	 */
	struct path source_path;
	struct path source_nofollow_path;
	struct inode *source_inode;
	struct kstat source_stat;
	struct kstat source_nofollow_stat;
	struct kstat visible_stat;
	unsigned long source_ino;
	unsigned long source_dev;
	unsigned long visible_ino;
	unsigned long visible_dev;
	unsigned long nofollow_visible_ino;
	unsigned long nofollow_visible_dev;
	umode_t source_mode;
	umode_t source_nofollow_mode;
	kuid_t source_uid;
	kgid_t source_gid;
	loff_t source_size;
	bool source_stat_valid;
	bool visible_stat_valid;
	/* A real node existed at src when the rule was installed.  Its DAC and
	 * timestamp metadata remains the visible template while source-backed
	 * type/size/allocation fields are refreshed dynamically. */
	bool preserve_visible_metadata;
	bool source_path_valid;
	bool source_nofollow_stat_valid;
	bool source_nofollow_path_valid;
	u32 rule_flags;
	u32 src_hash;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct kasumi_hide_entry {
	char *path;
	u32 path_hash;
	bool storage_managed;
	u32 rule_flags;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct kasumi_inject_entry {
	char *dir;
	struct inode *fop_inode;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct kasumi_xattr_sb_entry {
	struct super_block *sb;
	struct work_struct free_work;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct kasumi_merge_entry {
	u32 rule_flags;
	char *src;
	char *target;
	char *resolved_src;
	struct dentry *target_dentry;
	struct hlist_node node;
	struct rcu_head rcu;
};

struct kasumi_merge_target_node {
	struct list_head list;
	char *target;
	struct dentry *target_dentry;
};

struct kasumi_name_list {
	char *name;
	u64 ino;
	unsigned char type;
	struct list_head list;
};

struct kasumi_filldir_wrapper {
	struct dir_context wrap_ctx;
	struct dir_context *orig_ctx;
	struct dentry *parent_dentry;
	int dir_path_len;
	bool dir_has_hidden;
	const char *dir_path;
	bool dir_has_inject;
	bool inject_done;
	bool view_allowed;
	bool spoof_allowed;
	int merge_target_count;
	struct dentry *merge_target_dentries[KASUMI_MAX_MERGE_TARGETS];
	char dir_path_buf[KASUMI_ITERATE_PATH_BUF];
};

/*
 * get_vfs_caps_from_disk kretprobe state.  The exec-path file-capability read
 * lands on the visible vnode's dentry, whose synthetic inode carries no on-disk
 * security.capability.  We stash the source's parsed caps at vnode-create time
 * (sleepable) and replay them here (atomic): @have gates the replay, @caps is
 * the pre-parsed value, @out points at the caller's cpu_vfs_cap_data.
 */
struct kasumi_fscap_ri_data {
	bool have;
	struct cpu_vfs_cap_data *out;
	struct cpu_vfs_cap_data caps;
};

#endif /* _KASUMI_TYPES_H */
