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
		if (entry->path_hash != hash || strcmp(entry->path, src))
			continue;
		if (unbind && kasumi_dirhijack_enabled()) {
			ret = kasumi_dirhijack_del(src);
			if (ret && ret != -ENOENT)
				break;
			ret = 0;
		}
		hlist_del_rcu(&entry->node);
		atomic_dec(&kasumi_hide_count);
		if (inode && inode->i_mapping)
			clear_bit(AS_FLAGS_KASUMI_HIDE,
				  &inode->i_mapping->flags);
		call_rcu(&entry->rcu, kasumi_hide_entry_free_rcu);
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
	char *src;
	u32 hash;
	bool found = false;
	int ret;

	if (!path || path[0] != '/')
		return -EINVAL;
	src = kstrdup(path, GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	ret =
	    kasumi_resolve_rule_path(&src, &target_inode, &parent_inode, false);
	if (ret)
		goto hide_done;
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
	new_hide->rule_flags = rule_flags;
	new_hide->storage_managed = kasumi_hide_storage_parent(parent_inode);
	/* Do not publish a rule that the VFS cannot enforce. */
	ret = kasumi_dirhijack_hide(src, rule_flags);
	if (ret)
		goto hide_done;
	if (target_inode)
		kasumi_mark_inode_hidden(target_inode);
	if (parent_inode && parent_inode->i_mapping)
		set_bit(AS_FLAGS_KASUMI_DIR_HAS_HIDDEN,
			&parent_inode->i_mapping->flags);

	hash = full_name_hash(NULL, src, strlen(src));
	mutex_lock(&kasumi_config_mutex);
	hlist_for_each_entry(
	    hide_entry, &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)],
	    node)
	{
		if (hide_entry->path_hash == hash &&
		    !strcmp(hide_entry->path, src)) {
			WRITE_ONCE(hide_entry->storage_managed,
				   new_hide->storage_managed);
			WRITE_ONCE(hide_entry->rule_flags,
				   new_hide->rule_flags);
			found = true;
			break;
		}
	}
	if (!found) {
		unsigned long h1 =
		    jhash(src, strlen(src), 0) & (KASUMI_BLOOM_SIZE - 1);
		unsigned long h2 =
		    jhash(src, strlen(src), 1) & (KASUMI_BLOOM_SIZE - 1);

		new_hide->path_hash = hash;
		set_bit(h1, kasumi_hide_bloom);
		set_bit(h2, kasumi_hide_bloom);
		atomic_inc(&kasumi_hide_count);
		hlist_add_head_rcu(
		    &new_hide->node,
		    &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)]);
		new_hide = NULL;
		kasumi_log("hide rule: src=%s\n", src);
	}
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

static KASUMI_NOCFI int kasumi_dispatch_cmd(unsigned int cmd, void __user *arg)
{
	struct kasumi_syscall_arg req;
	struct kasumi_entry *entry;
	struct kasumi_hide_entry *hide_entry;
	struct kasumi_inject_entry *inject_entry;
	char *src = NULL, *target = NULL;
	u32 hash;
	bool found = false;
	int ret = 0;

	if (cmd == KSM_IOC_CLEAR_ALL) {
		kasumi_hide_rules_clear();
		mutex_lock(&kasumi_config_mutex);
		kasumi_cleanup_locked();
		mutex_unlock(&kasumi_config_mutex);
		kasumi_dirhijack_clear();
		kasumi_fop_override_clear();
		kasumi_sop_shadow_reap();
		kasumi_fake_mi_invalidate_all();
		rcu_barrier();
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
		struct kasumi_spoof_kstat __user *u =
		    (struct kasumi_spoof_kstat __user *)arg;
		struct kasumi_spoof_kstat *k;
		struct kasumi_spoof_kstat_entry *e, *existing = NULL;
		size_t plen;
		u32 phash = 0;
		bool have_path;
		struct path resolved;
		unsigned long auto_ino = 0;

		k = kmalloc(sizeof(*k), GFP_KERNEL);
		if (!k)
			return -ENOMEM;
		if (copy_from_user(k, u, sizeof(*k))) {
			kfree(k);
			return -EFAULT;
		}
		k->target_pathname[KSM_MAX_LEN_PATHNAME - 1] = '\0';
		have_path = (k->target_pathname[0] != '\0');

		/* Auto-resolve target_ino from path if userspace did not supply
		 * one. */
		if (have_path && k->target_ino == 0) {
			if (kern_path(k->target_pathname, LOOKUP_FOLLOW,
				      &resolved) == 0) {
				if (resolved.dentry &&
				    d_inode(resolved.dentry)) {
					struct inode *inode =
					    d_inode(resolved.dentry);

					auto_ino = (unsigned long)inode->i_ino;
					(void)kasumi_iop_mark_spoof(inode);
				}
				path_put(&resolved);
			}
			if (auto_ino)
				k->target_ino = auto_ino;
		}
		if (have_path && k->target_ino != 0) {
			if (kern_path(k->target_pathname, LOOKUP_FOLLOW,
				      &resolved) == 0) {
				if (resolved.dentry && d_inode(resolved.dentry))
					(void)kasumi_iop_mark_spoof(
					    d_inode(resolved.dentry));
				path_put(&resolved);
			}
		}

		if (!have_path && !k->target_ino) {
			k->err = -EINVAL;
			(void)copy_to_user(u, k, sizeof(*k));
			kfree(k);
			return -EINVAL;
		}

		if (have_path) {
			plen = strlen(k->target_pathname);
			phash = full_name_hash(NULL, k->target_pathname, plen);
		}

		mutex_lock(&kasumi_config_mutex);

		/* Look for existing entry by path, then by ino. */
		if (have_path) {
			hlist_for_each_entry(e,
					     &kasumi_spoof_kstat_path[hash_min(
						 phash, KASUMI_HASH_BITS)],
					     path_node)
			{
				if (e->path_hash == phash &&
				    e->target_pathname &&
				    strcmp(e->target_pathname,
					   k->target_pathname) == 0) {
					existing = e;
					break;
				}
			}
		}
		if (!existing && k->target_ino) {
			hlist_for_each_entry(
			    e,
			    &kasumi_spoof_kstat_ino[hash_min(k->target_ino,
							     KASUMI_HASH_BITS)],
			    ino_node)
			{
				if (e->target_ino == k->target_ino &&
				    e->target_dev == 0) {
					existing = e;
					break;
				}
			}
		}

		if (existing && cmd == KSM_IOC_ADD_SPOOF_KSTAT) {
			/* Idempotent ADD: treat as UPDATE. */
		}

		if (!existing) {
			e = kzalloc(sizeof(*e), GFP_KERNEL);
			if (!e) {
				mutex_unlock(&kasumi_config_mutex);
				k->err = -ENOMEM;
				(void)copy_to_user(u, k, sizeof(*k));
				kfree(k);
				return -ENOMEM;
			}
			if (have_path) {
				e->target_pathname =
				    kstrdup(k->target_pathname, GFP_KERNEL);
				if (!e->target_pathname) {
					kfree(e);
					mutex_unlock(&kasumi_config_mutex);
					k->err = -ENOMEM;
					(void)copy_to_user(u, k, sizeof(*k));
					kfree(k);
					return -ENOMEM;
				}
				e->path_hash = phash;
			}
			e->target_ino = k->target_ino;
			e->target_dev = 0;
			e->spoofed_ino = k->spoofed_ino;
			e->spoofed_dev = k->spoofed_dev;
			e->spoofed_nlink = k->spoofed_nlink;
			e->spoofed_size = k->spoofed_size;
			e->spoofed_atime_sec = k->spoofed_atime_sec;
			e->spoofed_atime_nsec = k->spoofed_atime_nsec;
			e->spoofed_mtime_sec = k->spoofed_mtime_sec;
			e->spoofed_mtime_nsec = k->spoofed_mtime_nsec;
			e->spoofed_ctime_sec = k->spoofed_ctime_sec;
			e->spoofed_ctime_nsec = k->spoofed_ctime_nsec;
			e->spoofed_blksize = k->spoofed_blksize;
			e->spoofed_blocks = k->spoofed_blocks;
			e->is_static = k->is_static;

			if (have_path)
				hlist_add_head_rcu(
				    &e->path_node,
				    &kasumi_spoof_kstat_path[hash_min(
					phash, KASUMI_HASH_BITS)]);
			if (e->target_ino)
				hlist_add_head_rcu(
				    &e->ino_node,
				    &kasumi_spoof_kstat_ino[hash_min(
					e->target_ino, KASUMI_HASH_BITS)]);
			atomic_inc(&kasumi_spoof_kstat_count);
			kasumi_log("spoof_kstat: add path=%s ino=%lu->%lu\n",
				   have_path ? k->target_pathname : "(none)",
				   e->target_ino, e->spoofed_ino);
		} else {
			/* Update fields in place; readers may see torn values
			 * briefly, acceptable for stat() spoof. */
			existing->spoofed_ino = k->spoofed_ino;
			existing->spoofed_dev = k->spoofed_dev;
			existing->spoofed_nlink = k->spoofed_nlink;
			existing->spoofed_size = k->spoofed_size;
			existing->spoofed_atime_sec = k->spoofed_atime_sec;
			existing->spoofed_atime_nsec = k->spoofed_atime_nsec;
			existing->spoofed_mtime_sec = k->spoofed_mtime_sec;
			existing->spoofed_mtime_nsec = k->spoofed_mtime_nsec;
			existing->spoofed_ctime_sec = k->spoofed_ctime_sec;
			existing->spoofed_ctime_nsec = k->spoofed_ctime_nsec;
			existing->spoofed_blksize = k->spoofed_blksize;
			existing->spoofed_blocks = k->spoofed_blocks;
			existing->is_static = k->is_static;

			/* If newly-resolved ino became available, link into ino
			 * table. */
			if (existing->target_ino == 0 && k->target_ino) {
				existing->target_ino = k->target_ino;
				hlist_add_head_rcu(
				    &existing->ino_node,
				    &kasumi_spoof_kstat_ino[hash_min(
					k->target_ino, KASUMI_HASH_BITS)]);
			}
			kasumi_log("spoof_kstat: update path=%s ino=%lu->%lu\n",
				   have_path ? k->target_pathname : "(none)",
				   existing->target_ino, existing->spoofed_ino);
		}

		mutex_unlock(&kasumi_config_mutex);

		k->err = 0;
		if (copy_to_user(u, k, sizeof(*k))) {
			kfree(k);
			return -EFAULT;
		}
		kfree(k);
		return 0;
	}

	if (cmd == KSM_IOC_ADD_MAPS_RULE) {
		struct kasumi_maps_rule __user *u =
		    (struct kasumi_maps_rule __user *)arg;
		struct kasumi_maps_rule k;
		struct kasumi_maps_rule_entry *e;

		if (copy_from_user(&k, u, sizeof(k)))
			return -EFAULT;
		e = kmalloc(sizeof(*e), GFP_KERNEL);
		if (!e) {
			k.err = -ENOMEM;
			if (copy_to_user(u, &k, sizeof(k)))
				return -EFAULT;
			return -ENOMEM;
		}
		e->target_ino = k.target_ino;
		e->target_dev = k.target_dev;
		e->spoofed_ino = k.spoofed_ino;
		e->spoofed_dev = k.spoofed_dev;
		strscpy(e->spoofed_pathname, k.spoofed_pathname,
			sizeof(e->spoofed_pathname));
		k.err = 0;
		if (copy_to_user(u, &k, sizeof(k))) {
			kfree(e);
			return -EFAULT;
		}
		mutex_lock(&kasumi_maps_mutex);
		list_add_tail(&e->list, &kasumi_maps_rules);
		mutex_unlock(&kasumi_maps_mutex);
		return 0;
	}

	if (cmd == KSM_IOC_CLEAR_MAPS_RULES) {
		struct kasumi_maps_rule_entry *e, *tmp;

		mutex_lock(&kasumi_maps_mutex);
		list_for_each_entry_safe (e, tmp, &kasumi_maps_rules, list) {
			list_del(&e->list);
			kfree(e);
		}
		mutex_unlock(&kasumi_maps_mutex);
		return 0;
	}

	if (cmd == KSM_IOC_SET_MOUNT_HIDE) {
		struct kasumi_mount_hide_arg a;
		if (copy_from_user(&a, arg, sizeof(a)))
			return -EFAULT;
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
		if (kasumi_proc_proxy_registered ||
		    kasumi_mount_hide_vfsmnt_registered ||
		    kasumi_mount_hide_mountinfo_registered)
			features |= KSM_FEATURE_MOUNT_HIDE;
		if (kasumi_proc_proxy_registered && kasumi_fake_mi_active())
			features |= KSM_FEATURE_FAKE_MOUNTINFO;
		if (kasumi_proc_proxy_registered)
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
		n = scnprintf(kbuf + written, buf_size - written,
			      "path: none\n");
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

		n = scnprintf(
		    kbuf + written, buf_size - written,
		    "selinuxfs (passthrough): access/context=%s\n"
		    "SELinux Guard: attr/current=%s delay=%uns "
		    "phase=post-EINVAL "
		    "spins=%llu\n",
		    kasumi_fake_selinuxfs_access_active() ? "shadow fop"
							  : "none",
		    kasumi_fake_selinuxfs_proc_attr_active()
			? "file-open/write proxy"
			: "none",
		    kasumi_fake_selinuxfs_delay_ns(),
		    (unsigned long long)kasumi_fake_selinuxfs_spin_count());
		written += n;

		n = scnprintf(kbuf + written, buf_size - written,
			      "mountinfo/mounts: %s\n",
			      kasumi_proc_proxy_registered
				  ? "fd-install fop proxy (isolated readers)"
				  : "none");
		written += n;
		n = scnprintf(
		    kbuf + written, buf_size - written, "fake mountinfo: %s\n",
		    kasumi_proc_proxy_registered && kasumi_fake_mi_active()
			? "shared mount pair, normalized propagation IDs"
			: "none");
		written += n;

		/* maps spoof */
		if (kasumi_proc_proxy_registered)
			n = scnprintf(kbuf + written, buf_size - written,
				      "maps: fd-install fop proxy\n");
		else
			n = scnprintf(kbuf + written, buf_size - written,
				      "maps: none\n");
		written += n;
		if (kasumi_statfs_view_available())
			n = scnprintf(kbuf + written, buf_size - written,
				      "statfs/fstatfs: path-aware VFS wrapper "
				      "(entries=%llu spoofs=%llu)\n",
				      (unsigned long long)atomic64_read(
					  &kasumi_hook_stats.statfs_entries),
				      (unsigned long long)atomic64_read(
					  &kasumi_hook_stats.statfs_spoofs));
		else
			n = scnprintf(kbuf + written, buf_size - written,
				      "statfs: none\n");
		written += n;

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

			if (kern_path(src, LOOKUP_FOLLOW, &mpath) == 0) {
				char *rbuf = kmalloc(PATH_MAX, GFP_KERNEL);
				if (rbuf) {
					char *res =
					    d_path(&mpath, rbuf, PATH_MAX);
					if (!IS_ERR(res) && res[0] == '/' &&
					    strcmp(res, src) != 0)
						resolved_src =
						    kstrdup(res, GFP_KERNEL);
					kfree(rbuf);
				}
				path_put(&mpath);
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
				if (strcmp(me->src, src) == 0 &&
				    strcmp(me->target, target) == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				me = kmalloc(sizeof(*me), GFP_KERNEL);
				if (me) {
					mat_src = kstrdup(src, GFP_KERNEL);
					mat_tgt = kstrdup(target, GFP_KERNEL);
					me->src = src;
					me->target = target;
					me->resolved_src = resolved_src;
					me->target_dentry = tgt_dentry;
					me->rule_flags = req.type;
					resolved_src = NULL;
					tgt_dentry = NULL;
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
				kasumi_log(
				    "add merge rule: src=%s, target=%s\n",
				    me->src, me->target);
				kasumi_add_inject_rule(
				    kstrdup(me->src, GFP_KERNEL));
				if (me->resolved_src)
					kasumi_add_inject_rule(kstrdup(
					    me->resolved_src, GFP_KERNEL));
				if (mat_src && mat_tgt)
					ret = kasumi_materialize_merge(
					    mat_src, mat_tgt, 0, req.type);
			}
			kfree(resolved_src);
			if (tgt_dentry)
				dput(tgt_dentry);
			kfree(mat_src);
			kfree(mat_tgt);
		}
		break;
	}

	case KSM_IOC_ADD_RULE: {
		u8 bypass_flag = (req.type & KSM_RULE_BYPASS_POLICY)
				     ? KASUMI_VNODE_F_BYPASS
				     : 0;
		char *parent_dir = NULL;
		char *resolved_src = NULL;
		char *visible_src = NULL;
		struct kasumi_entry *new_entry = NULL;
		struct path path;
		struct inode *target_inode = NULL;
		bool install_side_effects = true;
		char *tmp_buf;
		unsigned long dh_v_ino = 0;
		umode_t dh_src_mode = 0;
		bool dh_want = false;
		unsigned long dh_nofollow_ino = 0;
		bool dh_nofollow_valid = false;

		if (!src || !target) {
			ret = -EINVAL;
			break;
		}

		tmp_buf = kmalloc(PATH_MAX, GFP_KERNEL);
		if (!tmp_buf) {
			ret = -ENOMEM;
			break;
		}

		/* Try to resolve full path */
		if (kern_path(src, LOOKUP_FOLLOW, &path) == 0) {
			char *res = d_path(&path, tmp_buf, PATH_MAX);
			if (!IS_ERR(res)) {
				resolved_src = kstrdup(res, GFP_KERNEL);
				{
					char *ls = strrchr(res, '/');
					if (ls) {
						if (ls == res)
							parent_dir = kstrdup(
							    "/", GFP_KERNEL);
						else {
							size_t l = ls - res;
							parent_dir = kmalloc(
							    l + 1, GFP_KERNEL);
							if (parent_dir) {
								memcpy(
								    parent_dir,
								    res, l);
								parent_dir[l] =
								    '\0';
							}
						}
					}
				}
			}
			path_put(&path);
		} else {
			char *ls = strrchr(src, '/');
			if (ls) {
				size_t l = ls - src;
				char *p_str;

				if (ls == src) {
					p_str = kstrdup("/", GFP_KERNEL);
				} else {
					p_str = kmalloc(l + 1, GFP_KERNEL);
					if (p_str) {
						memcpy(p_str, src, l);
						p_str[l] = '\0';
					}
				}
				if (p_str) {
					if (kern_path(p_str, LOOKUP_FOLLOW,
						      &path) == 0) {
						char *res = d_path(
						    &path, tmp_buf, PATH_MAX);
						if (!IS_ERR(res)) {
							if (!strcmp(res, "/")) {
								resolved_src =
								    kstrdup(
									ls,
									GFP_KERNEL);
							} else {
								size_t rl =
								    strlen(res);
								size_t nl =
								    strlen(ls);

								resolved_src =
								    kmalloc(
									rl +
									    nl +
									    1,
									GFP_KERNEL);
								if (resolved_src) {
									strcpy(
									    resolved_src,
									    res);
									strcat(
									    resolved_src,
									    ls);
								}
							}
							parent_dir = kstrdup(
							    res, GFP_KERNEL);
						}
						path_put(&path);
					}
					kfree(p_str);
				}
			}
		}
		kfree(tmp_buf);
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
		new_entry->rule_flags = req.type;
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

		install_side_effects = kasumi_store_upsert(new_entry);
		new_entry = NULL;

		if (install_side_effects && parent_dir) {
			kasumi_add_inject_rule(parent_dir);
			parent_dir = NULL;
		}
		if (install_side_effects && target &&
		    kern_path(target, LOOKUP_FOLLOW, &path) == 0) {
			if (path.dentry && d_inode(path.dentry)) {
				target_inode = d_inode(path.dentry);
				ihold(target_inode);
			}
			path_put(&path);
		}
		if (target_inode) {
			(void)kasumi_iop_mark_spoof(target_inode);
			iput(target_inode);
		}

		/* Tier 3: when the lookup hijack is enabled, also register this
		 * rule's visible child so VFS lookup resolves it to a Kasumi
		 * virtual inode. v1 handled regular-file sources; Slice 3 adds
		 * symlink sources, which are registered as a symlink vnode
		 * pinned to the link itself (nofollow) so lstat/readlink see
		 * the link and the kernel follows it natively. */
		if (dh_want && kasumi_dirhijack_enabled()) {
			struct path dsrc;

			ret = -ENOENT;

			if (dh_nofollow_valid &&
			    kern_path(target, 0, &dsrc) == 0) {
				struct inode *di = d_inode(dsrc.dentry);

				if (di && S_ISLNK(di->i_mode))
					ret = kasumi_dirhijack_add(
					    src, &dsrc, dh_nofollow_ino,
					    (KASUMI_VNODE_F_LNK) | bypass_flag);
				path_put(&dsrc);
			} else if (S_ISREG(dh_src_mode) &&
				   kern_path(target, LOOKUP_FOLLOW, &dsrc) ==
				       0) {
				ret = kasumi_dirhijack_add(src, &dsrc, dh_v_ino,
							   (0) | bypass_flag);
				path_put(&dsrc);
			} else if (S_ISDIR(dh_src_mode) &&
				   kern_path(target, LOOKUP_FOLLOW, &dsrc) ==
				       0) {
				/* Slice 4c: a directory-source redirect
				 * resolves to a Kasumi directory vnode whose
				 * lookup/iterate delegate to the pinned source
				 * dir. */
				ret = kasumi_dirhijack_add(
				    src, &dsrc, dh_v_ino,
				    (KASUMI_VNODE_F_DIR) | bypass_flag);
				path_put(&dsrc);
			} else if ((S_ISCHR(dh_src_mode) ||
				    S_ISBLK(dh_src_mode) ||
				    S_ISFIFO(dh_src_mode)) &&
				   kasumi_device_sources_enabled &&
				   kern_path(target, LOOKUP_FOLLOW, &dsrc) ==
				       0) {
				/* char/blk/fifo source: resolves to a special
				 * vnode wrapper (S_IFREG inode to clear
				 * may_open_dev on the nodev visible mount;
				 * .open delegates to the real device/fifo,
				 * getattr projects the source type+rdev). */
				ret = kasumi_dirhijack_add(
				    src, &dsrc, dh_v_ino,
				    (KASUMI_VNODE_F_SPECIAL) | bypass_flag);
				path_put(&dsrc);
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
		ret = kasumi_add_hide_rule(src, req.type);
		break;

	case KSM_IOC_HIDE_OVERLAY_XATTRS:
		ret = src ? kasumi_overlay_xattr_mark(src) : -EINVAL;
		break;

	case KSM_IOC_DEL_RULE: {
		struct inode *del_inode = NULL;

		ret = kasumi_resolve_delete_path(&src, &del_inode);
		if (ret)
			break;
		/* Keep indexed state intact if VFS unbinding fails. */
		if (kasumi_dirhijack_enabled()) {
			ret = kasumi_dirhijack_del(src);
			if (ret && ret != -ENOENT) {
				if (del_inode)
					iput(del_inode);
				break;
			}
			ret = 0;
		}

		hash = full_name_hash(NULL, src, strlen(src));
		mutex_lock(&kasumi_config_mutex);

		hlist_for_each_entry(
		    entry, &kasumi_paths[hash_min(hash, KASUMI_HASH_BITS)],
		    node)
		{
			if (entry->src_hash == hash &&
			    strcmp(entry->src, src) == 0) {
				kasumi_clear_inode_flags_for_path(
				    entry->target, AS_FLAGS_KASUMI_SPOOF_KSTAT);
				hlist_del_rcu(&entry->node);
				atomic_dec(&kasumi_rule_count);
				kasumi_log("del rule: src=%s\n", src);
				call_rcu(&entry->rcu, kasumi_entry_free_rcu);
				goto del_done;
			}
		}
		hlist_for_each_entry(
		    hide_entry,
		    &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node)
		{
			if (hide_entry->path_hash == hash &&
			    strcmp(hide_entry->path, src) == 0) {
				hlist_del_rcu(&hide_entry->node);
				atomic_dec(&kasumi_hide_count);
				kasumi_log("del rule: src=%s\n", src);
				call_rcu(&hide_entry->rcu,
					 kasumi_hide_entry_free_rcu);
				goto del_done;
			}
		}
		hlist_for_each_entry(
		    inject_entry,
		    &kasumi_inject_dirs[hash_min(hash, KASUMI_HASH_BITS)], node)
		{
			if (strcmp(inject_entry->dir, src) == 0) {
				kasumi_fop_put(inject_entry->fop_inode, true);
				inject_entry->fop_inode = NULL;
				hlist_del_rcu(&inject_entry->node);
				atomic_dec(&kasumi_rule_count);
				kasumi_log("del rule: src=%s\n", src);
				call_rcu(&inject_entry->rcu,
					 kasumi_inject_entry_free_rcu);
				goto del_done;
			}
		}
	del_done:
		mutex_unlock(&kasumi_config_mutex);
		kasumi_prune_inject_parent(src);
		if (del_inode) {
			if (del_inode->i_mapping)
				clear_bit(AS_FLAGS_KASUMI_HIDE,
					  &del_inode->i_mapping->flags);
			iput(del_inode);
		} else {
			/* A cached hidden negative may have blocked the first
			 * lookup. */
			kasumi_clear_inode_flags_for_path(src,
							  AS_FLAGS_KASUMI_HIDE);
		}
		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	kfree(src);
	kfree(target);
	return ret;
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
long kasumi_handle_ioctl(unsigned int cmd, void __user *arg)
{
	long ret;

	/* Report rejected initialization without requiring an active VFS. */
	if (cmd == KSM_IOC_GET_CONFLICTS || cmd == KSM_IOC_ACK_CONFLICTS)
		return kasumi_conflict_ioctl(cmd, arg);

	ret = kasumi_control_begin();

	if (ret)
		return ret;

	mutex_lock(&kasumi_mutation_mutex);
	atomic_long_set(&kasumi_ioctl_tgid, (long)task_tgid_vnr(current));
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
	atomic_long_set(&kasumi_ioctl_tgid, 0);
	if (ret >= 0 && ksu_sucompat_vfs_enabled() &&
	    (cmd == KSM_IOC_SET_ENABLED || cmd == KSM_IOC_CLEAR_ALL ||
	     cmd == KSM_IOC_ADD_RULE || cmd == KSM_IOC_ADD_MERGE_RULE ||
	     cmd == KSM_IOC_DEL_RULE)) {
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
