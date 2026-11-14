#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) &&                           \
    !defined(arch_ftrace_get_regs)
#define arch_ftrace_get_regs(fregs) (NULL)
#endif
#include <linux/kprobes.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/jhash.h>
#include <linux/kdev_t.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/sched/task.h>
#include <linux/fs_struct.h>
#include <linux/dirent.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/anon_inodes.h>
#include <linux/fcntl.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/seq_file.h>
#include <uapi/linux/magic.h>
#ifndef EROFS_SUPER_MAGIC
#define EROFS_SUPER_MAGIC 0xe0f5e1e2
#endif
#include <asm/unistd.h>
#include "kasumi_runtime.h"
#include "kasumi_dirhijack.h"
#include "kasumi_hide_rules.h"
#include "kasumi_vnode.h"
#include "kasumi_bootstrap.h"
#include "kasumi_conflict.h"
#include "feature/sucompat_vfs.h"
#include "kasumi_store.h"
#include "kasumi_batch.h"
#include "kasumi_live_rules.h"
#include "kasumi_transaction.h"
#include "kasumi_maps_rules.h"
#include "kasumi_entrypoints.h"
#include "kasumi_path_policy.h"
#include "kasumi_overlay.h"
#include "kasumi_proc_read_hooks.h"
#include "kasumi_vfs_hooks.h"
#include "kasumi_vfs_view.h"
#include "kasumi_fop_bridge.h"
#include "kasumi_iop_override.h"
#include "kasumi_fop_override.h"
#include "kasumi_sop_shadow.h"
#include "kasumi_fake_mountinfo.h"
#include "kasumi_mount_view.h"
#include "kasumi_proc_seq.h"
#include "kasumi_fake_selinuxfs_access.h"

static int KASUMI_NOCFI kasumi_resolve_rule_path(char **pathname,
						 struct inode **target_out,
						 struct inode **parent_out,
						 bool follow_leaf)
{
	struct path path;
	struct inode *target = NULL, *parent = NULL;
	char *buffer, *parent_name = NULL, *resolved = NULL, *text;
	const char *leaf = NULL;
	int ret;

	if (!pathname || !*pathname || (*pathname)[0] != '/')
		return -EINVAL;
	buffer = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	ret = kern_path(*pathname, follow_leaf ? LOOKUP_FOLLOW : 0, &path);
	if (ret == -ENOENT) {
		char *slash;

		parent_name = kstrdup(*pathname, GFP_KERNEL);
		if (!parent_name) {
			ret = -ENOMEM;
			goto out;
		}
		slash = strrchr(parent_name, '/');
		leaf = *pathname + (slash - parent_name) + 1;
		if (!*leaf) {
			ret = -EINVAL;
			goto out;
		}
		if (slash == parent_name)
			slash[1] = '\0';
		else
			*slash = '\0';
		ret = kern_path(parent_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
				&path);
		if (ret == -ENOENT) {
			/* Keep the stored key deletable after its parent
			 * disappears. */
			ret = 0;
			goto out;
		}
	}
	if (ret)
		goto out;
	text = d_path(&path, buffer, PATH_MAX);
	if (IS_ERR(text)) {
		ret = PTR_ERR(text);
		goto out_path;
	}
	if (text[0] != '/') {
		ret = -EINVAL;
		goto out_path;
	}
	if (leaf) {
		size_t prefix = strcmp(text, "/") ? strlen(text) : 0;
		size_t length = strlen(leaf);

		if (prefix + length + 2 > PATH_MAX) {
			ret = -ENAMETOOLONG;
			goto out_path;
		}
		resolved = kmalloc(prefix + length + 2, GFP_KERNEL);
		if (resolved) {
			memcpy(resolved, text, prefix);
			resolved[prefix] = '/';
			memcpy(resolved + prefix + 1, leaf, length + 1);
		}
		parent = d_inode(path.dentry);
	} else {
		resolved = kstrdup(text, GFP_KERNEL);
		target = d_inode(path.dentry);
		parent = d_inode(path.dentry->d_parent);
	}
	if (!resolved) {
		ret = -ENOMEM;
		goto out_path;
	}
	if (target_out && target) {
		ihold(target);
		*target_out = target;
	}
	if (parent_out && parent) {
		ihold(parent);
		*parent_out = parent;
	}
	kfree(*pathname);
	*pathname = resolved;
out_path:
	path_put(&path);
out:
	kfree(parent_name);
	kfree(buffer);
	return ret;
}

static int kasumi_control_bind(const char *visible, const struct path *source,
			       unsigned long ino, u8 flags)
{
	struct kasumi_dirhijack_batch *batch = kasumi_batch_dir();

	if (batch)
		return kasumi_dirhijack_batch_add(batch, visible, source, ino,
						 flags, false);
	return kasumi_dirhijack_add(visible, source, ino, flags);
}

static int kasumi_control_hide(const char *visible, u32 flags)
{
	struct kasumi_dirhijack_batch *batch = kasumi_batch_dir();

	if (batch)
		return kasumi_dirhijack_batch_hide(batch, visible, flags);
	return kasumi_dirhijack_hide(visible, flags);
}

static int kasumi_control_unbind(const char *visible)
{
	struct kasumi_dirhijack_batch *batch = kasumi_batch_dir();

	if (batch)
		return kasumi_dirhijack_batch_del(batch, visible);
	return kasumi_dirhijack_del(visible);
}

static int KASUMI_NOCFI kasumi_resolve_delete_path(char **pathname,
						   struct inode **target_out)
{
	struct kasumi_hide_entry *entry;
	u32 hash;
	bool hidden = false;
	int ret;

	ret = kasumi_resolve_rule_path(pathname, target_out, NULL, false);
	if (ret)
		return ret;
	hash = full_name_hash(NULL, *pathname, strlen(*pathname));
	mutex_lock(&kasumi_config_mutex);
	hlist_for_each_entry(
	    entry, &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node)
	{
		if (entry->path_hash == hash &&
		    !strcmp(entry->path, *pathname)) {
			hidden = true;
			break;
		}
	}
	mutex_unlock(&kasumi_config_mutex);
	if (hidden)
		return 0;
	if (*target_out) {
		iput(*target_out);
		*target_out = NULL;
	}
	return kasumi_resolve_rule_path(pathname, target_out, NULL, true);
}

// A replacement has either replaced the VFS child or must unbind an old hide.
int KASUMI_NOCFI kasumi_forget_hide_rule(const char *path, bool unbind)
{
	struct kasumi_hide_entry *entry;
	struct inode *inode = NULL;
	char *src;
	u32 hash;
	int ret;
	u64 generation = kasumi_batch_generation();

	if (!atomic_read(&kasumi_hide_count))
		return 0;
	src = kstrdup(path, GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	ret = kasumi_resolve_rule_path(&src, &inode, NULL, false);
	if (ret)
		goto out;
	hash = full_name_hash(NULL, src, strlen(src));
	mutex_lock(&kasumi_config_mutex);
	hlist_for_each_entry(
	    entry, &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node)
	{
		if (!kasumi_rule_visible(&entry->life,
				generation ? generation : kasumi_rules_generation()) ||
		    entry->path_hash != hash || strcmp(entry->path, src))
			continue;
		if (unbind && kasumi_dirhijack_enabled()) {
			ret = kasumi_control_unbind(src);
			if (ret && ret != -ENOENT)
				break;
			ret = 0;
		}
		if (generation) {
			kasumi_batch_expire(&entry->life, KASUMI_BATCH_HIDE);
		} else {
			hlist_del_rcu(&entry->node);
			atomic_dec(&kasumi_hide_count);
			if (inode && inode->i_mapping)
				clear_bit(AS_FLAGS_KASUMI_HIDE,
					  &inode->i_mapping->flags);
			kasumi_rule_index_remove(&entry->life);
			kasumi_live_rules_untrack(&entry->life);
			call_rcu(&entry->rcu, kasumi_hide_entry_free_rcu);
		}
		break;
	}
	mutex_unlock(&kasumi_config_mutex);
out:
	if (inode)
		iput(inode);
	kfree(src);
	return ret;
}

int KASUMI_NOCFI kasumi_add_hide_rule(const char *path, u32 rule_flags)
{
	struct kasumi_hide_entry *new_hide = NULL, *hide_entry;
	struct inode *target_inode = NULL, *parent_inode = NULL;
	struct path projected_parent = {};
	char *src;
	u32 hash;
	u64 generation = kasumi_batch_generation();
	int ret;
	bool projected;

	if (!path || path[0] != '/')
		return -EINVAL;
	src = kstrdup(path, GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	ret = kasumi_rule_projected_parent(src,
		generation ? generation : kasumi_rules_generation(),
		&projected_parent);
	if (ret < 0)
		goto hide_done;
	projected = ret > 0;
	if (projected_parent.dentry) {
		parent_inode = d_inode(projected_parent.dentry);
		ihold(parent_inode);
		path_put(&projected_parent);
	}
	if (!projected) {
		ret = kasumi_resolve_rule_path(&src, &target_inode, &parent_inode,
					       false);
		if (ret)
			goto hide_done;
	}
	new_hide = kzalloc(sizeof(*new_hide), GFP_KERNEL);
	if (!new_hide) {
		ret = -ENOMEM;
		goto hide_done;
	}
	new_hide->path = kstrdup(src, GFP_KERNEL);
	if (!new_hide->path) {
		ret = -ENOMEM;
		goto hide_done;
	}
	new_hide->life.first_generation = generation;
	ret = kasumi_rule_sequence_alloc(&new_hide->sequence);
	if (ret)
		goto hide_done;
	new_hide->rule_flags = rule_flags;
	new_hide->storage_managed = kasumi_hide_storage_parent(parent_inode);
	if (!projected) {
		ret = kasumi_control_hide(src, rule_flags);
		if (ret)
			goto hide_done;
		if (target_inode)
			kasumi_mark_inode_hidden(target_inode);
		if (parent_inode && parent_inode->i_mapping)
			set_bit(AS_FLAGS_KASUMI_DIR_HAS_HIDDEN,
				&parent_inode->i_mapping->flags);
	}
	ret = 0;

	hash = full_name_hash(NULL, src, strlen(src));
	mutex_lock(&kasumi_config_mutex);
	hlist_for_each_entry(hide_entry,
		&kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node) {
		if (!kasumi_rule_visible(&hide_entry->life,
				generation ? generation : kasumi_rules_generation()) ||
		    hide_entry->path_hash != hash ||
		    strcmp(hide_entry->path, src))
			continue;
		if (generation) {
			kasumi_batch_expire(&hide_entry->life, KASUMI_BATCH_HIDE);
		} else {
			hlist_del_rcu(&hide_entry->node);
			atomic_dec(&kasumi_hide_count);
			kasumi_rule_index_remove(&hide_entry->life);
			kasumi_live_rules_untrack(&hide_entry->life);
			call_rcu(&hide_entry->rcu, kasumi_hide_entry_free_rcu);
		}
		break;
	}
	new_hide->path_hash = hash;
	set_bit(jhash(src, strlen(src), 0) & (KASUMI_BLOOM_SIZE - 1),
		kasumi_hide_bloom);
	set_bit(jhash(src, strlen(src), 1) & (KASUMI_BLOOM_SIZE - 1),
		kasumi_hide_bloom);
	kasumi_batch_track(&new_hide->life, KASUMI_BATCH_HIDE);
	atomic_inc(&kasumi_hide_count);
	hlist_add_head_rcu(&new_hide->node,
		&kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)]);
	new_hide = NULL;
	mutex_unlock(&kasumi_config_mutex);
hide_done:
	if (new_hide) {
		kfree(new_hide->path);
		kfree(new_hide);
	}
	if (target_inode)
		iput(target_inode);
	if (parent_inode)
		iput(parent_inode);
	kfree(src);
	return ret;
}

static int kasumi_apply_path_rule_owned(unsigned int cmd, char *src,
				       char *target, u32 flags);

static KASUMI_NOCFI int kasumi_dispatch_cmd(unsigned int cmd, void __user *arg)
{
	struct kasumi_syscall_arg req;
	struct kasumi_entry *entry;
	struct kasumi_hide_entry *hide_entry;
	struct kasumi_inject_entry *inject_entry;
	char *src = NULL, *target = NULL;
	int ret = 0;

	if (cmd == KSM_IOC_CLEAR_ALL) {
		kasumi_live_rules_clear();
		kasumi_hide_rules_clear();
		mutex_lock(&kasumi_config_mutex);
		kasumi_cleanup_locked();
		mutex_unlock(&kasumi_config_mutex);
		kasumi_dirhijack_clear();
		kasumi_fop_override_clear();
		kasumi_sop_shadow_reap();
		kasumi_fake_mi_invalidate_all();
		kasumi_store_drain();
		return 0;
	}

	if (cmd == KSM_IOC_GET_VERSION) {
		int ver = KSM_PROTOCOL_VERSION;
		if (copy_to_user(arg, &ver, sizeof(ver)))
			return -EFAULT;
		return 0;
	}

	if (cmd == KSM_IOC_GET_ENABLED) {
		int enabled = smp_load_acquire(&kasumi_enabled);

		return copy_to_user(arg, &enabled, sizeof(enabled)) ? -EFAULT
								    : 0;
	}

	if (cmd == KSM_IOC_SET_DEBUG) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		kasumi_debug_enabled = !!val;
		kasumi_log("debug mode %s\n",
			   kasumi_debug_enabled ? "enabled" : "disabled");
		return 0;
	}

	if (cmd == KSM_IOC_SET_STEALTH) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		kasumi_stealth_enabled = !!val;
		kasumi_log("stealth mode %s\n",
			   kasumi_stealth_enabled ? "enabled" : "disabled");
		return 0;
	}

	if (cmd == KSM_IOC_SET_ENABLED) {
		int val;
		if (copy_from_user(&val, arg, sizeof(val)))
			return -EFAULT;
		if (val) {
			mutex_lock(&kasumi_config_mutex);
			if (READ_ONCE(kasumi_enabled)) {
				mutex_unlock(&kasumi_config_mutex);
				return 0;
			}
			smp_store_release(&kasumi_enabled, true);
			mutex_unlock(&kasumi_config_mutex);
		} else {
			mutex_lock(&kasumi_config_mutex);
			if (!READ_ONCE(kasumi_enabled)) {
				mutex_unlock(&kasumi_config_mutex);
				return 0;
			}
			smp_store_release(&kasumi_enabled, false);
			mutex_unlock(&kasumi_config_mutex);
		}
		kasumi_log("Kasumi %s\n",
			   READ_ONCE(kasumi_enabled) ? "enabled" : "disabled");
		kasumi_hide_rules_changed();
		return 0;
	}

	if (cmd == KSM_IOC_REORDER_MNT_ID) {
		/* struct mnt_namespace/mount not exposed to LKM; only KPM
		 * (built-in) supports this */
		return -EOPNOTSUPP;
	}

	if (cmd == KSM_IOC_LIST_RULES) {
		struct kasumi_syscall_list_arg list_arg;
		struct kasumi_xattr_sb_entry *sb_entry;
		struct kasumi_merge_entry *merge_entry;
		char *kbuf;
		size_t buf_size, written = 0;
		int bkt;

		if (copy_from_user(&list_arg, arg, sizeof(list_arg)))
			return -EFAULT;

		buf_size = list_arg.size;
		if (buf_size > 64 * 1024)
			buf_size = 64 * 1024;

		kbuf = kzalloc(buf_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		rcu_read_lock();
		written +=
		    scnprintf(kbuf + written, buf_size - written,
			      "Kasumi Protocol: %d\n", KSM_PROTOCOL_VERSION);
		written +=
		    scnprintf(kbuf + written, buf_size - written,
			      "Kasumi Enabled: %d\n", kasumi_enabled ? 1 : 0);
		hash_for_each_rcu(kasumi_paths, bkt, entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "add %s %s %d\n", entry->src,
					     entry->target, entry->rule_flags);
		}
		hash_for_each_rcu(kasumi_hide_paths, bkt, hide_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "hide %s %u\n", hide_entry->path,
					     READ_ONCE(hide_entry->rule_flags));
		}
		hash_for_each_rcu(kasumi_inject_dirs, bkt, inject_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(kbuf + written, buf_size - written,
					     "inject %s\n", inject_entry->dir);
		}
		hash_for_each_rcu(kasumi_merge_dirs, bkt, merge_entry, node)
		{
			if (written >= buf_size)
				break;
			written += scnprintf(
			    kbuf + written, buf_size - written,
			    "merge %s %s %u\n", merge_entry->src,
			    merge_entry->target, merge_entry->rule_flags);
		}
		hash_for_each_rcu(kasumi_xattr_sbs, bkt, sb_entry, node)
		{
			if (written >= buf_size)
				break;
			written +=
			    scnprintf(kbuf + written, buf_size - written,
				      "hide_xattr_sb %p\n", sb_entry->sb);
		}
		if (kasumi_feature_enabled_mask & KSM_FEATURE_MOUNT_HIDE) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "mount_hide enabled\n");
		}
		if (kasumi_feature_enabled_mask & KSM_FEATURE_MAPS_SPOOF) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "maps_spoof enabled\n");
		}
		if (kasumi_feature_enabled_mask & KSM_FEATURE_STATFS_SPOOF) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "statfs_spoof enabled\n");
		}
		if (kasumi_feature_enabled_mask & KSM_FEATURE_SELINUX_FIX) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "selinux_fix enabled\n");
		}
		if (kasumi_stealth_enabled) {
			if (written < buf_size)
				written += scnprintf(kbuf + written,
						     buf_size - written,
						     "stealth enabled\n");
		}
		rcu_read_unlock();

		if (copy_to_user(list_arg.buf, kbuf, written)) {
			kfree(kbuf);
			return -EFAULT;
		}
		list_arg.size = written;
		if (copy_to_user(arg, &list_arg, sizeof(list_arg))) {
			kfree(kbuf);
			return -EFAULT;
		}
		kfree(kbuf);
		return 0;
	}

	if (cmd == KSM_IOC_SET_MIRROR_PATH) {
		/* ABI slot 14 is intentionally retained, but pure virtual mode
		 * no longer owns or consumes a mirror/workdir path. */
		return -EOPNOTSUPP;
	}

	if (cmd == KSM_IOC_ADD_SPOOF_KSTAT ||
	    cmd == KSM_IOC_UPDATE_SPOOF_KSTAT) {
		struct kasumi_spoof_kstat *rule;

		rule = memdup_user(arg, sizeof(*rule));
		if (IS_ERR(rule))
			return PTR_ERR(rule);
		rule->target_pathname[KSM_MAX_LEN_PATHNAME - 1] = '\0';
		ret = kasumi_spoof_kstat_upsert(rule);
		rule->err = ret;
		if (copy_to_user(arg, rule, sizeof(*rule)))
			ret = -EFAULT;
		kfree(rule);
		return ret;
	}

	if (cmd == KSM_IOC_ADD_MAPS_RULE) {
		struct kasumi_maps_rule rule;

		if (copy_from_user(&rule, arg, sizeof(rule)))
			return -EFAULT;
		ret = kasumi_maps_rules_upsert(&rule);
		rule.err = ret;
		if (copy_to_user(arg, &rule, sizeof(rule)))
			return -EFAULT;
		return ret;
	}

	if (cmd == KSM_IOC_CLEAR_MAPS_RULES) {
		kasumi_maps_rules_clear();
		return 0;
	}

	if (cmd == KSM_IOC_SET_MOUNT_HIDE) {
		struct kasumi_mount_hide_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !kasumi_mount_projection_available())
			return -EOPNOTSUPP;
		if (a.enable)
			kasumi_feature_enabled_mask |= KSM_FEATURE_MOUNT_HIDE;
		else
			kasumi_feature_enabled_mask &= ~KSM_FEATURE_MOUNT_HIDE;
		kasumi_fake_mi_invalidate_all();
		kasumi_log("mount hide %s\n",
			   a.enable ? "enabled" : "disabled");
		/* path_pattern reserved for future custom hide rules */
		return 0;
	}

	if (cmd == KSM_IOC_SET_MAPS_SPOOF) {
		struct kasumi_maps_spoof_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !kasumi_proc_maps_available())
			return -EOPNOTSUPP;
		if (a.enable)
			kasumi_feature_enabled_mask |= KSM_FEATURE_MAPS_SPOOF;
		else
			kasumi_feature_enabled_mask &= ~KSM_FEATURE_MAPS_SPOOF;
		/* reserved for future inline rule */
		return 0;
	}

	if (cmd == KSM_IOC_SET_STATFS_SPOOF) {
		struct kasumi_statfs_spoof_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
		if (a.enable && !kasumi_statfs_view_available())
			return -EOPNOTSUPP;
		if (a.enable)
			kasumi_feature_enabled_mask |= KSM_FEATURE_STATFS_SPOOF;
		else
			kasumi_feature_enabled_mask &=
			    ~KSM_FEATURE_STATFS_SPOOF;
		/* path/spoof_f_type reserved for future custom mappings */
		return 0;
	}

	if (cmd == KSM_IOC_SELINUX_FIX) {
		int enable;

		if (copy_from_user(&enable, arg, sizeof(enable)))
			return -EFAULT;
		if (enable && !kasumi_fake_selinuxfs_proc_attr_active())
			return -EOPNOTSUPP;
		if (enable)
			kasumi_feature_enabled_mask |= KSM_FEATURE_SELINUX_FIX;
		else
			kasumi_feature_enabled_mask &= ~KSM_FEATURE_SELINUX_FIX;
		return 0;
	}

	if (cmd == KSM_IOC_GET_FEATURES) {
		int features = 0;
		if (kasumi_hide_rules_available())
			features |= KSM_FEATURE_MANAGED_HIDE;
		features |= KSM_FEATURE_KSTAT_SPOOF;
		features |= KSM_FEATURE_MERGE_DIR;
		features |= KSM_FEATURE_RULE_BYPASS;
		features |= KSM_FEATURE_SAFE_RULE_RETIRE;
		if (kasumi_overlay_xattr_available())
			features |= KSM_FEATURE_OVERLAY_XATTR_HIDE;
		if (kasumi_mount_projection_available())
			features |= KSM_FEATURE_MOUNT_HIDE | KSM_FEATURE_FAKE_MOUNTINFO;
		if (kasumi_proc_maps_available())
			features |= KSM_FEATURE_MAPS_SPOOF;
		if (kasumi_statfs_view_available())
			features |= KSM_FEATURE_STATFS_SPOOF;
		if (kasumi_fake_selinuxfs_proc_attr_active())
			features |= KSM_FEATURE_SELINUX_FIX;
		if (copy_to_user(arg, &features, sizeof(features)))
			return -EFAULT;
		return 0;
	}

	if (cmd == KSM_IOC_GET_HOOKS) {
		struct kasumi_syscall_list_arg list_arg;
		char *kbuf;
		size_t buf_size, written = 0;
		int n;

		if (copy_from_user(&list_arg, arg, sizeof(list_arg)))
			return -EFAULT;

		buf_size = list_arg.size;
		if (buf_size > 4096)
			buf_size = 4096;

		kbuf = kzalloc(buf_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		written += scnprintf(kbuf + written, buf_size - written,
				     "control: KernelSU fd\n");

		/* Path redirect: served entirely through the VFS lookup/vnode
		 * layer; no syscall dispatcher or sys_enter tracepoint remains.
		 */
		n = scnprintf(kbuf + written, buf_size - written, "path: %s\n",
			      kasumi_dirhijack_enabled() ? "VFS lookup"
							 : "unavailable");
		written += n;
		written += scnprintf(kbuf + written, buf_size - written,
				     "fop_inodes: live=%d retiring=%d\n",
				     kasumi_fop_live_count(),
				     kasumi_fop_retiring_count());

		{
			dev_t vnode_dev = kasumi_vnode_device();

			n = scnprintf(
			    kbuf + written, buf_size - written,
			    "vnode: live=%u allocated=%llu dev=%u:%u\n",
			    kasumi_vnode_live(),
			    (unsigned long long)kasumi_vnode_allocated(),
			    MAJOR(vnode_dev), MINOR(vnode_dev));
			written += n;
		}
		n = scnprintf(
		    kbuf + written, buf_size - written, "overlay xattrs: %s\n",
		    kasumi_overlay_xattr_available() ? "VFS get/list filter"
						     : "none");
		written += n;

		n = scnprintf(kbuf + written, buf_size - written,
			      "vfs: getattr=iop readdir=fop xattrs=%s\n",
			      kasumi_overlay_xattr_available() ? "VFS-filter"
							       : "none");
		written += n;

		written += scnprintf(kbuf + written, buf_size - written,
			"source xattrs: get/list/set/remove=%u\n"
			"live source rules: available=%u desired=%u error=%d\n",
			kasumi_source_xattr_available(),
			kasumi_live_rules_available(), kasumi_live_rules_count(),
			kasumi_live_rules_error());
		written += scnprintf(
		    kbuf + written, buf_size - written,
		    "selinuxfs (passthrough): access/context=%s\n"
		    "SELinux Guard: attr/current=%s delay=%uns "
		    "phase=post-EINVAL spins=%llu\n",
		    kasumi_fake_selinuxfs_access_active() ? "shadow fop"
							  : "none",
		    kasumi_fake_selinuxfs_proc_attr_active()
			? "file-open/write proxy"
			: "none",
		    kasumi_fake_selinuxfs_delay_ns(),
		    (unsigned long long)kasumi_fake_selinuxfs_spin_count());
		written += scnprintf(kbuf + written, buf_size - written,
			"mount projection: installed=%#llx required=%#llx complete=%u\n"
			"maps/smaps: native seq adapter=%u capabilities=%#llx\n"
			"statfs/fstatfs: path-aware VFS wrapper=%u entries=%llu projections=%llu\n",
			(unsigned long long)kasumi_mount_view_capabilities(),
			(unsigned long long)kasumi_mount_view_required_capabilities(),
			kasumi_mount_projection_available(),
			kasumi_proc_seq_available(),
			(unsigned long long)kasumi_proc_seq_capabilities(),
			kasumi_statfs_view_available(),
			(unsigned long long)atomic64_read(&kasumi_hook_stats.statfs_entries),
			(unsigned long long)atomic64_read(&kasumi_hook_stats.statfs_spoofs));

		list_arg.size = written;
		if (copy_to_user(arg, &list_arg, sizeof(list_arg))) {
			kfree(kbuf);
			return -EFAULT;
		}
		if (written && copy_to_user(list_arg.buf, kbuf, written)) {
			kfree(kbuf);
			return -EFAULT;
		}
		kfree(kbuf);
		return 0;
	}

	/* Commands that use kasumi_syscall_arg */
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.type & ~KSM_RULE_FLAGS_MASK)
		return -EINVAL;
	if (req.type && cmd != KSM_IOC_ADD_RULE &&
	    cmd != KSM_IOC_ADD_MERGE_RULE && cmd != KSM_IOC_HIDE_RULE)
		return -EINVAL;

	if (req.src) {
		src = strndup_user(req.src, PAGE_SIZE);
		if (IS_ERR(src))
			return PTR_ERR(src);
	}
	if (req.target) {
		target = strndup_user(req.target, PAGE_SIZE);
		if (IS_ERR(target)) {
			kfree(src);
			return PTR_ERR(target);
		}
	}
	if (src && ksu_sucompat_vfs_reserved_path(src)) {
		kfree(src);
		kfree(target);
		return -EPERM;
	}

	return kasumi_apply_path_rule_owned(cmd, src, target, req.type);
}

static int kasumi_canonical_rule_path(const char *name, char **resolved,
				      char **parent)
{
	struct path path;
	char *buffer, *text, *lookup = NULL;
	const char *suffix = NULL;
	int error;

#ifndef KASUMI_EMBEDDED
	if (!kasumi_kern_path || !kasumi_d_path)
		return -EOPNOTSUPP;
#endif
#ifdef KASUMI_EMBEDDED
	error = kern_path(name, 0, &path);
#else
	error = kasumi_kern_path(name, 0, &path);
#endif
	/* A cached synthetic symlink belongs to the visible namespace. Its
	 * backing link target must never become the configuration key. */
	if (!error && S_ISLNK(d_inode(path.dentry)->i_mode) &&
	    !kasumi_vnode_is_ours(d_inode(path.dentry))) {
		path_put(&path);
#ifdef KASUMI_EMBEDDED
		error = kern_path(name, LOOKUP_FOLLOW, &path);
#else
		error = kasumi_kern_path(name, LOOKUP_FOLLOW, &path);
#endif
	}
	if (error == -ENOENT) {
		suffix = strrchr(name, '/');
		if (!suffix)
			return -EINVAL;
		lookup = suffix == name ? kstrdup("/", GFP_KERNEL) :
			 kstrndup(name, suffix - name, GFP_KERNEL);
		if (!lookup)
			return -ENOMEM;
#ifdef KASUMI_EMBEDDED
		error = kern_path(lookup, LOOKUP_FOLLOW, &path);
#else
		error = kasumi_kern_path(lookup, LOOKUP_FOLLOW, &path);
#endif
		kfree(lookup);
		if (error == -ENOENT)
			return 0;
	}
	if (error)
		return error;
	buffer = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buffer) {
		error = -ENOMEM;
		goto put_path;
	}
#ifdef KASUMI_EMBEDDED
	text = d_path(&path, buffer, PATH_MAX);
#else
	text = kasumi_d_path(&path, buffer, PATH_MAX);
#endif
	if (IS_ERR(text)) {
		error = PTR_ERR(text);
		goto free_buffer;
	}
	if (text[0] != '/') {
		error = -ENOENT;
		goto free_buffer;
	}
	*resolved = suffix ? kasprintf(GFP_KERNEL, "%s%s",
				     !strcmp(text, "/") ? "" : text, suffix) :
			    kstrdup(text, GFP_KERNEL);
	if (!*resolved) {
		error = -ENOMEM;
		goto free_buffer;
	}
	if (strlen(*resolved) >= PATH_MAX) {
		error = -ENAMETOOLONG;
		goto free_resolved;
	}
	if (parent) {
		const char *slash = strrchr(*resolved, '/');

		*parent = slash == *resolved ? kstrdup("/", GFP_KERNEL) :
			  kstrndup(*resolved, slash - *resolved, GFP_KERNEL);
		if (!*parent) {
			error = -ENOMEM;
			goto free_resolved;
		}
	}
	goto free_buffer;
free_resolved:
	kfree(*resolved);
	*resolved = NULL;
free_buffer:
	kfree(buffer);
put_path:
#ifdef KASUMI_EMBEDDED
	path_put(&path);
#else
	kasumi_path_put(&path);
#endif
	return error;
}

static int kasumi_apply_path_rule_owned(unsigned int cmd, char *src,
				       char *target, u32 flags)
{
	struct kasumi_entry *entry;
	struct kasumi_hide_entry *hide_entry;
	struct kasumi_inject_entry *inject_entry;
	struct kasumi_batch *batch = NULL;
	u64 generation;
	u32 hash;
	bool found = false;
	int ret = 0;

	if (!kasumi_batch_generation()) {
		batch = kasumi_batch_begin();
		if (IS_ERR(batch)) {
			ret = PTR_ERR(batch);
			batch = NULL;
			goto apply_done;
		}
	}
	generation = kasumi_batch_generation();
	ret = kasumi_live_rules_record(kasumi_batch_live(), cmd, src, target, flags);
	if (ret)
		goto apply_done;

	switch (cmd) {
	case KSM_IOC_ADD_MERGE_RULE: {
		struct kasumi_merge_entry *me;
		char *mat_src = NULL, *mat_tgt = NULL;

		if (!src || !target) {
			ret = -EINVAL;
			break;
		}

		ret = kasumi_check_merge_target(target);
		if (ret)
			break;

		/* Resolve symlinks: d_absolute_path in iterate_dir returns
		 * canonical paths (e.g. /product/overlay), while userspace
		 * sends symlink paths (e.g. /system/product/overlay). Store the
		 * canonical form as resolved_src for iterate_dir matching. */
		{
			char *resolved_src = NULL;
			struct dentry *tgt_dentry = NULL;
			struct path mpath;

			ret = kasumi_canonical_rule_path(src, &resolved_src, NULL);
			if (ret)
				goto merge_done;
			if (resolved_src && !strcmp(resolved_src, src)) {
				kfree(resolved_src);
				resolved_src = NULL;
			}
			if (kern_path(target, LOOKUP_FOLLOW, &mpath) == 0) {
				tgt_dentry = dget(mpath.dentry);
				path_put(&mpath);
			}

			hash = full_name_hash(NULL, src, strlen(src));
			mutex_lock(&kasumi_config_mutex);

			hlist_for_each_entry(me,
					     &kasumi_merge_dirs[hash_min(
						 hash, KASUMI_HASH_BITS)],
					     node)
			{
				if (kasumi_rule_visible(&me->life, generation) &&
				    strcmp(me->src, src) == 0 &&
				    strcmp(me->target, target) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				me = kzalloc(sizeof(*me), GFP_KERNEL);
				if (me) {
					mat_src = kstrdup(src, GFP_KERNEL);
					mat_tgt = kstrdup(target, GFP_KERNEL);
					me->src = src;
					me->target = target;
					me->resolved_src = resolved_src;
					me->target_dentry = tgt_dentry;
					me->life.first_generation = generation;
					me->rule_flags = flags;
					resolved_src = NULL;
					tgt_dentry = NULL;
					kasumi_batch_track(&me->life, KASUMI_BATCH_MERGE);
					hlist_add_head_rcu(
					    &me->node,
					    &kasumi_merge_dirs[hash_min(
						hash, KASUMI_HASH_BITS)]);
					src = NULL;
					target = NULL;
				} else {
					ret = -ENOMEM;
				}
			} else {
				ret = -EEXIST;
			}
			mutex_unlock(&kasumi_config_mutex);
			if (!found && !ret) {
				if (!mat_src || !mat_tgt)
					ret = -ENOMEM;
				if (!ret)
					ret = kasumi_add_inject_rule(
						kstrdup(me->src, GFP_KERNEL));
				if (!ret && me->resolved_src)
					ret = kasumi_add_inject_rule(
						kstrdup(me->resolved_src, GFP_KERNEL));
				if (!ret)
					ret = kasumi_materialize_merge(
						mat_src, mat_tgt, 0, flags);
			}
merge_done:
			kfree(resolved_src);
			if (tgt_dentry)
				dput(tgt_dentry);
			kfree(mat_src);
			kfree(mat_tgt);
		}
		break;
	}

	case KSM_IOC_ADD_RULE: {
		u8 bypass_flag = (flags & KSM_RULE_BYPASS_POLICY)
				     ? KASUMI_VNODE_F_BYPASS
				     : 0;
		char *parent_dir = NULL;
		char *resolved_src = NULL;
		char *visible_src = NULL;
		struct kasumi_entry *new_entry = NULL;
		struct path captured_source = {}, captured_nofollow = {};
		int install_side_effects;
		unsigned long dh_v_ino = 0;
		umode_t dh_src_mode = 0;
		bool dh_want = false;
		unsigned long dh_nofollow_ino = 0;
		bool dh_nofollow_valid = false;

		if (!src || !target) {
			ret = -EINVAL;
			break;
		}

		ret = kasumi_canonical_rule_path(src, &resolved_src, &parent_dir);
		if (ret)
			goto add_rule_done;
		visible_src = kstrdup(src, GFP_KERNEL);
		if (!visible_src) {
			kfree(resolved_src);
			ret = -ENOMEM;
			goto add_rule_done;
		}

		if (resolved_src) {
			kfree(src);
			src = resolved_src;
		}

		hash = full_name_hash(NULL, src, strlen(src));
		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry) {
			ret = -ENOMEM;
			goto add_rule_done;
		}
		new_entry->src = kstrdup(src, GFP_KERNEL);
		new_entry->target = kstrdup(target, GFP_KERNEL);
		new_entry->rule_flags = flags;
		new_entry->src_hash = hash;
		if (!new_entry->src || !new_entry->target) {
			ret = -ENOMEM;
			goto add_rule_done;
		}
		ret = kasumi_entry_capture_source(new_entry, target);
		if (ret)
			goto add_rule_done;
		dh_v_ino = new_entry->visible_ino;
		dh_src_mode = new_entry->source_mode;
		dh_want = true;
		/* Captured before the config_mutex section because new_entry is
		 * set to NULL once it is inserted; these feed the symlink
		 * dirhijack branch. */
		dh_nofollow_valid = new_entry->source_nofollow_path_valid;
		dh_nofollow_ino = new_entry->nofollow_visible_ino;
		/* Keep the store, metadata hook and child binding on the same
		 * captured objects even if the source pathname is replaced. */
		if (new_entry->source_path_valid) {
			captured_source = new_entry->source_path;
			path_get(&captured_source);
		}
		if (new_entry->source_nofollow_path_valid) {
			captured_nofollow = new_entry->source_nofollow_path;
			path_get(&captured_nofollow);
		}

		install_side_effects = kasumi_store_upsert(new_entry);
		if (install_side_effects < 0) {
			ret = install_side_effects;
			goto add_rule_done;
		}
		new_entry = NULL;

		if (install_side_effects && parent_dir) {
			ret = kasumi_add_inject_rule(parent_dir);
			parent_dir = NULL;
			if (ret)
				goto add_rule_done;
		}

		/* Tier 3: when the lookup hijack is enabled, also register this
		 * rule's visible child so VFS lookup resolves it to a Kasumi
		 * virtual inode. v1 handled regular-file sources; Slice 3 adds
		 * symlink sources, which are registered as a symlink vnode
		 * pinned to the link itself (nofollow) so lstat/readlink see
		 * the link and the kernel follows it natively. */
		if (dh_want && kasumi_dirhijack_enabled()) {
			ret = -ENOENT;
			if (dh_nofollow_valid) {
				ret = kasumi_control_bind(src, &captured_nofollow,
					dh_nofollow_ino, KASUMI_VNODE_F_LNK | bypass_flag);
			} else if (S_ISREG(dh_src_mode)) {
				ret = kasumi_control_bind(src, &captured_source,
					dh_v_ino, bypass_flag);
			} else if (S_ISDIR(dh_src_mode)) {
				ret = kasumi_control_bind(src, &captured_source,
					dh_v_ino, KASUMI_VNODE_F_DIR | bypass_flag);
			} else if ((S_ISCHR(dh_src_mode) || S_ISBLK(dh_src_mode) ||
				    S_ISFIFO(dh_src_mode)) &&
				   kasumi_device_sources_enabled) {
				ret = kasumi_control_bind(src, &captured_source,
					dh_v_ino, KASUMI_VNODE_F_SPECIAL | bypass_flag);
			}
		}

		if (!ret)
			ret = kasumi_forget_hide_rule(src, false);
		if (!ret && strcmp(visible_src, src))
			ret = kasumi_forget_hide_rule(visible_src, true);

		/* Exact rules are injected when the visible dentry is absent.
		 * Existing real names are de-duplicated by the directory view,
		 * so no hide marker is needed for either form. */
	add_rule_done:
		if (captured_source.dentry)
			path_put(&captured_source);
		if (captured_nofollow.dentry)
			path_put(&captured_nofollow);
		if (new_entry) {
			kasumi_entry_release_source(new_entry);
			kfree(new_entry->src);
			kfree(new_entry->target);
			kfree(new_entry->source_canonical);
			kfree(new_entry);
		}
		kfree(visible_src);
		kfree(parent_dir);
		break;
	}

	case KSM_IOC_HIDE_RULE:
		ret = kasumi_add_hide_rule(src, flags);
		break;

	case KSM_IOC_HIDE_OVERLAY_XATTRS:
		ret = src ? kasumi_overlay_xattr_mark_at(src, generation) : -EINVAL;
		break;

	case KSM_IOC_DEL_RULE: {
		struct kasumi_merge_entry *merge;
		struct inode *del_inode = NULL;

		ret = kasumi_resolve_delete_path(&src, &del_inode);
		if (ret)
			break;
		ret = kasumi_control_unbind(src);
		if (ret == -ENOENT)
			ret = 0;
		if (del_inode)
			iput(del_inode);
		if (ret)
			break;
		hash = full_name_hash(NULL, src, strlen(src));
		mutex_lock(&kasumi_config_mutex);
		hlist_for_each_entry(entry,
			&kasumi_paths[hash_min(hash, KASUMI_HASH_BITS)], node) {
			if (kasumi_rule_visible(&entry->life, generation) &&
			    entry->src_hash == hash && !strcmp(entry->src, src))
				kasumi_batch_expire(&entry->life, KASUMI_BATCH_PATH);
		}
		hlist_for_each_entry(hide_entry,
			&kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node) {
			if (kasumi_rule_visible(&hide_entry->life, generation) &&
			    hide_entry->path_hash == hash && !strcmp(hide_entry->path, src))
				kasumi_batch_expire(&hide_entry->life, KASUMI_BATCH_HIDE);
		}
		hlist_for_each_entry(inject_entry,
			&kasumi_inject_dirs[hash_min(hash, KASUMI_HASH_BITS)], node) {
			if (kasumi_rule_visible(&inject_entry->life, generation) &&
			    !strcmp(inject_entry->dir, src))
				kasumi_batch_expire(&inject_entry->life, KASUMI_BATCH_INJECT);
		}
		hlist_for_each_entry(merge,
			&kasumi_merge_dirs[hash_min(hash, KASUMI_HASH_BITS)], node) {
			if (kasumi_rule_visible(&merge->life, generation) &&
			    !strcmp(merge->src, src))
				kasumi_batch_expire(&merge->life, KASUMI_BATCH_MERGE);
		}
		mutex_unlock(&kasumi_config_mutex);
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

apply_done:
	kasumi_live_rules_end_record();
	kfree(src);
	kfree(target);
	if (batch)
		kasumi_batch_finish(batch, ret == 0);
	return ret;
}

int kasumi_apply_path_rule(unsigned int command, const char *visible,
			  const char *source, u32 flags)
{
	char *visible_copy, *source_copy = NULL;

	if (!visible)
		return -EINVAL;
	visible_copy = kstrdup(visible, GFP_KERNEL);
	if (!visible_copy)
		return -ENOMEM;
	if (source) {
		source_copy = kstrdup(source, GFP_KERNEL);
		if (!source_copy) {
			kfree(visible_copy);
			return -ENOMEM;
		}
	}
	return kasumi_apply_path_rule_owned(command, visible_copy, source_copy, flags);
}

/* ======================================================================
 * Part 16: Ioctl Handler
 * ====================================================================== */

/*
 * Rule updates have side effects outside kasumi_config_mutex (inode/dentry
 * overrides and injected-directory state). Serialize the control plane so a
 * concurrent CLEAR_ALL cannot return while an older update is still applying
 * those side effects or re-enable the module afterwards.
 */
long kasumi_handle_ioctl(const struct file *file, unsigned int cmd,
			 void __user *arg)
{
	long ret;
	u64 generation;

	if (_IOC_TYPE(cmd) != KSM_IOC_MAGIC)
		return -EPROTONOSUPPORT;

	/* Report rejected initialization without requiring an active VFS. */
	if (cmd == KSM_IOC_GET_CONFLICTS || cmd == KSM_IOC_ACK_CONFLICTS)
		return kasumi_conflict_ioctl(cmd, arg);

	ret = kasumi_control_begin();

	if (ret)
		return ret;

	mutex_lock(&kasumi_mutation_mutex);
	atomic_long_set(&kasumi_ioctl_tgid, (long)task_tgid_vnr(current));
	generation = kasumi_rules_generation();
	if (kasumi_transaction_command(cmd)) {
		ret = kasumi_transaction_ioctl(file, cmd, arg);
		goto out;
	}
	switch (cmd) {
	case KSM_IOC_USER_HIDE_UPSERT:
	case KSM_IOC_USER_HIDE_DELETE:
	case KSM_IOC_USER_HIDE_QUERY:
	case KSM_IOC_USER_HIDE_RETRY:
	case KSM_IOC_USER_HIDE_CLEAR:
		ret = kasumi_hide_rules_ioctl(cmd, arg);
		break;
	case KSM_IOC_GET_VERSION:
	case KSM_IOC_GET_ENABLED:
	case KSM_IOC_SET_ENABLED:
	case KSM_IOC_ADD_RULE:
	case KSM_IOC_DEL_RULE:
	case KSM_IOC_HIDE_RULE:
	case KSM_IOC_CLEAR_ALL:
	case KSM_IOC_LIST_RULES:
	case KSM_IOC_SET_DEBUG:
	case KSM_IOC_REORDER_MNT_ID:
	case KSM_IOC_SET_STEALTH:
	case KSM_IOC_HIDE_OVERLAY_XATTRS:
	case KSM_IOC_ADD_MERGE_RULE:
	case KSM_IOC_SET_MIRROR_PATH:
	case KSM_IOC_GET_HOOKS:
	case KSM_IOC_ADD_MAPS_RULE:
	case KSM_IOC_CLEAR_MAPS_RULES:
	case KSM_IOC_GET_FEATURES:
	case KSM_IOC_SET_MOUNT_HIDE:
	case KSM_IOC_SET_MAPS_SPOOF:
	case KSM_IOC_SET_STATFS_SPOOF:
	case KSM_IOC_SELINUX_FIX:
	case KSM_IOC_ADD_SPOOF_KSTAT:
	case KSM_IOC_UPDATE_SPOOF_KSTAT:
		ret = kasumi_dispatch_cmd(cmd, arg);
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	if (kasumi_ioctl_changes_rules(cmd) &&
	    generation == kasumi_rules_generation())
		kasumi_rules_changed();
	atomic_long_set(&kasumi_ioctl_tgid, 0);
	if (ret >= 0 && ksu_sucompat_vfs_enabled() &&
	    (cmd == KSM_IOC_SET_ENABLED || cmd == KSM_IOC_CLEAR_ALL ||
	     cmd == KSM_IOC_ADD_RULE || cmd == KSM_IOC_ADD_MERGE_RULE ||
	     cmd == KSM_IOC_DEL_RULE || cmd == KSM_IOC_COMMIT_TRANSACTION)) {
		int refresh = ksu_sucompat_vfs_refresh();
		if (refresh)
			pr_warn(
			    "kasumi: failed to rebind internal su node: %d\n",
			    refresh);
	}
	mutex_unlock(&kasumi_mutation_mutex);
	kasumi_control_end();
	return ret;
}
