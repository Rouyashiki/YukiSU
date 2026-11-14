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
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/xarray.h>
#include <uapi/linux/magic.h>
#ifndef EROFS_SUPER_MAGIC
#define EROFS_SUPER_MAGIC 0xe0f5e1e2
#endif
#include <asm/unistd.h>
#include "kasumi_runtime.h"
#include "kasumi_batch.h"
#include "kasumi_dir_view.h"
#include "kasumi_vnode.h"
#include "kasumi_store.h"
#include "kasumi_path_policy.h"
#include "kasumi_dirhijack.h"
#include "kasumi_hide_rules.h"
#include "policy/allowlist.h"

#define KASUMI_MEDIA_RW_GID 1023

/* ======================================================================
 * Part 11: Core Logic - Privileged Check / Allowlist
 * ====================================================================== */

bool kasumi_is_privileged_process(void)
{
	return uid_eq(current_uid(), GLOBAL_ROOT_UID);
}

static enum kasumi_policy_scope kasumi_policy_scope_for_uid(uid_t uid)
{
	if (is_isolated_process(uid) ||
	    (uid % PER_USER_RANGE >= 90000 &&
	     uid % PER_USER_RANGE < FIRST_ISOLATED_UID))
		return KASUMI_POLICY_SCOPE_SPOOF;
	if (!is_appuid(uid))
		return KASUMI_POLICY_SCOPE_VIEW;
	return ksu_uid_should_umount(uid) ? KASUMI_POLICY_SCOPE_SPOOF
					  : KASUMI_POLICY_SCOPE_VIEW;
}

enum kasumi_policy_scope kasumi_policy_current_scope(void)
{
	if (kasumi_hide_rules_resolving() || kasumi_vfs_internal_current() ||
	    !kasumi_config_enabled())
		return KASUMI_POLICY_SCOPE_NONE;
	return kasumi_policy_scope_for_uid(__kuid_val(task_uid(current)));
}

bool kasumi_rule_scope_active(void)
{
	long tgid = atomic_long_read(&kasumi_ioctl_tgid);

	return kasumi_config_enabled() &&
	       !kasumi_vfs_internal_current() &&
	       !kasumi_hide_rules_resolving() &&
	       (tgid <= 0 || tgid != (long)task_tgid_vnr(current));
}

bool kasumi_rule_current_sees(u32 flags)
{
	return kasumi_rule_scope_active() &&
	       ((flags & KSM_RULE_BYPASS_POLICY) ||
		kasumi_policy_current_is_view_target());
}

bool kasumi_policy_current_is_view_target(void)
{
	return kasumi_policy_current_scope() == KASUMI_POLICY_SCOPE_VIEW;
}

bool kasumi_hide_storage_parent(const struct inode *parent)
{
	return parent && (parent->i_sb->s_magic == FUSE_SUPER_MAGIC ||
			  gid_eq(READ_ONCE(parent->i_gid),
				 KGIDT_INIT(KASUMI_MEDIA_RW_GID)));
}

bool kasumi_hide_scope_allowed(bool storage_managed, u32 flags)
{
	long ioctl_tgid;
	uid_t uid = __kuid_val(current_uid());

	if (!kasumi_rule_scope_active())
		return false;
	if (flags & KSM_RULE_BYPASS_POLICY)
		return true;

	if (!kasumi_config_enabled() ||
	    kasumi_is_privileged_process())
		return false;
	if (is_appuid(uid) && ksu_is_allow_uid(uid))
		return false;
	/* Storage services need the backing view, not a global hide exemption.
	 */
	if (storage_managed && in_group_p(KGIDT_INIT(KASUMI_MEDIA_RW_GID)))
		return false;
	ioctl_tgid = atomic_long_read(&kasumi_ioctl_tgid);
	return ioctl_tgid <= 0 || ioctl_tgid != (long)task_tgid_vnr(current);
}

bool kasumi_policy_current_is_hide_target(const struct inode *parent)
{
	return kasumi_hide_scope_allowed(kasumi_hide_storage_parent(parent), 0);
}

bool kasumi_rule_current_hides(const struct inode *parent, u32 flags)
{
	return kasumi_hide_scope_allowed(kasumi_hide_storage_parent(parent),
					 flags);
}

bool kasumi_policy_current_is_spoof_target(void)
{
	return kasumi_policy_current_scope() == KASUMI_POLICY_SCOPE_SPOOF;
}

bool kasumi_policy_uid_is_spoof_target(uid_t uid)
{
	return kasumi_policy_scope_for_uid(uid) == KASUMI_POLICY_SCOPE_SPOOF;
}

bool kasumi_policy_current_is_isolated(void)
{
	uid_t appid = __kuid_val(task_uid(current)) % PER_USER_RANGE;

	return appid >= 90000 && appid <= LAST_ISOLATED_UID;
}

#define KASUMI_SYMLINK_LIMIT 40

static bool kasumi_relative_path_safe(const char *path)
{
	const char *component = path;

	while (component && *component) {
		const char *slash = strchr(component, '/');
		size_t length =
		    slash ? (size_t)(slash - component) : strlen(component);

		if ((length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.'))
			return false;
		component = slash ? slash + 1 : NULL;
	}
	return true;
}

static bool KASUMI_NOCFI kasumi_rule_get_source_flags_depth(
    const char *pathname, unsigned int lookup_flags,
    struct kasumi_rule_source *source, unsigned int symlink_depth);

void kasumi_project_visible_stat(struct kstat *result,
				 const struct kstat *source_stat,
				 const struct kstat *visible_template,
				 bool preserve_visible_metadata,
				 unsigned long visible_ino,
				 unsigned long visible_dev)
{
	if (!result || !source_stat)
		return;
	if (preserve_visible_metadata && visible_template) {
		*result = *visible_template;
		result->mode = (source_stat->mode & S_IFMT) |
			       (visible_template->mode & ~S_IFMT);
		result->rdev = source_stat->rdev;
		result->size = source_stat->size;
		result->blocks = source_stat->blocks;
		result->blksize = source_stat->blksize;
		result->result_mask |=
		    source_stat->result_mask &
		    (STATX_TYPE | STATX_MODE | STATX_SIZE | STATX_BLOCKS);
	} else {
		*result = *source_stat;
	}
	result->ino = visible_ino;
	result->dev = visible_dev;
}

void kasumi_rule_source_put(struct kasumi_rule_source *source)
{
	if (source->path.dentry)
		path_put(&source->path);
	memset(&source->path, 0, sizeof(source->path));
	kasumi_identity_put(source->identity);
	source->identity = NULL;
}

static bool kasumi_rule_refresh_exact_source(struct kasumi_rule_source *source)
{
	struct kstat source_stat;
	int ret;

	if (!source || !source->path.dentry || !source->path.mnt)
		return false;
	memset(&source_stat, 0, sizeof(source_stat));
	ret = kasumi_vfs_getattr_unprojected(&source->path, &source_stat,
					     STATX_BASIC_STATS | STATX_BTIME,
					     AT_STATX_SYNC_AS_STAT);
	if (ret) {
		kasumi_rule_source_put(source);
		source->stat_valid = false;
		source->error = ret;
		return false;
	}
	/* The pinned path fixes object identity across rename/unlink, but
	 * mutable inode metadata must retain ordinary VFS behaviour after rule
	 * install. */
	source->source_mode = source_stat.mode;
	kasumi_project_visible_stat(&source->stat, &source_stat, &source->stat,
				    source->preserve_visible_metadata,
				    source->visible_ino, source->visible_dev);
	source->stat_valid = true;
	return true;
}

static bool kasumi_rule_source_from_path(struct path *resolved,
	struct kasumi_rule_source *source, struct kasumi_identity_domain *domain)
{
	struct inode *inode;
	struct kstat stat;
	int ret;

	inode = resolved && resolved->dentry ? d_inode(resolved->dentry) : NULL;
	if (!inode) {
		path_put(resolved);
		source->error = -ENOENT;
		return false;
	}
	memset(&stat, 0, sizeof(stat));
	ret = kasumi_vfs_getattr_unprojected(resolved, &stat,
					     STATX_BASIC_STATS | STATX_BTIME,
					     AT_STATX_SYNC_AS_STAT);
	if (ret) {
		path_put(resolved);
		source->error = ret;
		return false;
	}
	ret = kasumi_identity_domain_source_get(domain, inode->i_sb->s_dev,
		inode->i_ino, GFP_NOFS, &source->identity);
	if (ret) {
		path_put(resolved);
		source->error = ret;
		return false;
	}
	source->path = *resolved;
	source->stat = stat;
	source->visible_ino = kasumi_identity_ino(source->identity);
	/* Scheme A: dynamic (directory/merge) sources have no cached visible
	 * dev. Publish the captured /system dev — a real device and the correct
	 * one for the common /system masquerade — rather than the anonymous
	 * vnode minor. */
	source->visible_dev =
	    kasumi_system_dev ? kasumi_system_dev : kasumi_vnode_device();
	source->stat.ino = source->visible_ino;
	source->stat.dev = source->visible_dev;
	source->source_mode = stat.mode;
	source->stat_valid = true;
	source->preserve_visible_metadata = false;
	return true;
}

static KASUMI_NOCFI int kasumi_read_link_target(const struct path *link,
						char *target,
						size_t target_size)
{
	struct delayed_call done = {};
	const char *value;
	size_t length;
	int ret = 0;

	if (!link || !link->dentry || !target || target_size < 2)
		return -EOPNOTSUPP;
	value = vfs_get_link(link->dentry, &done);
	if (IS_ERR(value)) {
		ret = PTR_ERR(value);
		goto out;
	}
	if (!value) {
		ret = -EIO;
		goto out;
	}
	length = strnlen(value, target_size);
	if (length >= target_size) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	memcpy(target, value, length + 1);
out:
	do_delayed_call(&done);
	return ret;
}

static bool KASUMI_NOCFI kasumi_follow_absolute_link(
    const char *target, const char *remaining, unsigned int lookup_flags,
    struct kasumi_rule_source *source, unsigned int symlink_depth)
{
	struct path resolved;
	char *next;
	size_t target_len;
	size_t remaining_len;
	int ret;

	if (lookup_flags & LOOKUP_BENEATH) {
		source->error = -EXDEV;
		return false;
	}
	if (symlink_depth >= KASUMI_SYMLINK_LIMIT) {
		source->error = -ELOOP;
		return false;
	}
	target_len = strlen(target);
	remaining_len = remaining ? strlen(remaining) : 0;
	if (target_len + remaining_len + 1 > KSM_MAX_LEN_PATHNAME) {
		source->error = -ENAMETOOLONG;
		return false;
	}
	next = kmalloc(target_len + remaining_len + 1, GFP_KERNEL);
	if (!next) {
		source->error = -ENOMEM;
		return false;
	}
	memcpy(next, target, target_len);
	if (remaining_len) {
		if (next[target_len - 1] == '/' && remaining[0] == '/') {
			memcpy(next + target_len, remaining + 1, remaining_len);
		} else {
			memcpy(next + target_len, remaining, remaining_len + 1);
		}
	} else {
		next[target_len] = '\0';
	}
	if (kasumi_rule_get_source_flags_depth(next, lookup_flags, source,
					       symlink_depth + 1)) {
		kfree(next);
		return true;
	}
	if (source->error) {
		kfree(next);
		return false;
	}
	if (kasumi_should_hide(next)) {
		source->error = -ENOENT;
		kfree(next);
		return false;
	}
	ret = kern_path(next, lookup_flags, &resolved);
	kfree(next);
	if (ret) {
		source->error = ret;
		return false;
	}
	return kasumi_rule_source_from_path(&resolved, source, NULL);
}

static bool KASUMI_NOCFI kasumi_directory_rule_source(
    const char *pathname, size_t path_len, unsigned int lookup_flags,
    struct kasumi_rule_source *source, unsigned int symlink_depth)
{
	u64 generation;
	struct kasumi_entry *entry;
	struct path directory = {};
	struct kasumi_identity_domain *domain = NULL;
	bool found;
	struct path resolved;
	char *relative;
	char *cursor;
	size_t prefix_len = path_len;
	bool pinned = false;
	int ret;

	while (prefix_len > 1) {
		size_t slash = prefix_len;
		u32 hash;

		while (slash > 0 && pathname[slash - 1] != '/')
			slash--;
		if (slash <= 1)
			break;
		prefix_len = slash - 1;
		if (!kasumi_relative_path_safe(pathname + prefix_len + 1))
			return false;
		if (!test_bit(jhash(pathname, (u32)prefix_len, 0) &
				  (KASUMI_BLOOM_SIZE - 1),
			      kasumi_path_bloom) ||
		    !test_bit(jhash(pathname, (u32)prefix_len, 1) &
				  (KASUMI_BLOOM_SIZE - 1),
			      kasumi_path_bloom))
			continue;
		hash = full_name_hash(NULL, pathname, prefix_len);
		rcu_read_lock();
		generation = kasumi_rules_generation();
		hlist_for_each_entry_rcu(
		    entry, &kasumi_paths[hash_min(hash, KASUMI_HASH_BITS)],
		    node)
		{
			if (!kasumi_rule_visible(&entry->life, generation))
				continue;
			if (!kasumi_rule_current_sees(entry->rule_flags))
				continue;
			if (entry->src_hash != hash ||
			    strlen(entry->src) != prefix_len ||
			    memcmp(entry->src, pathname, prefix_len) != 0 ||
			    !entry->source_path_valid ||
			    !S_ISDIR(entry->source_mode))
				continue;
			domain = entry->source_domain;
			kasumi_identity_domain_get(domain);
			directory = entry->source_path;
			path_get(&directory);
			pinned = true;
			break;
		}
		rcu_read_unlock();
		if (pinned)
			break;
	}
	if (!pinned)
		return false;
	relative = kstrdup(pathname + prefix_len + 1, GFP_KERNEL);
	if (!relative) {
		path_put(&directory);
		kasumi_identity_domain_put(domain);
		source->error = -ENOMEM;
		return false;
	}
	ret = vfs_path_lookup(directory.dentry, directory.mnt, relative,
			      lookup_flags | LOOKUP_NO_SYMLINKS, &resolved);
	if (!ret) {
		path_put(&directory);
		kfree(relative);
		found = kasumi_rule_source_from_path(&resolved, source, domain);
		kasumi_identity_domain_put(domain);
		return found;
	}
	if (ret != -ELOOP || (lookup_flags & LOOKUP_NO_SYMLINKS)) {
		source->error = ret;
		goto out_relative;
	}
	{
		cursor = relative;
		while (*cursor) {
			struct path probe;
			char target[KSM_MAX_LEN_PATHNAME];
			char *slash = strchr(cursor, '/');
			bool final = !slash;
			char saved = '\0';

			if (slash) {
				saved = *slash;
				*slash = '\0';
			}
			ret = vfs_path_lookup(
			    directory.dentry, directory.mnt, relative,
			    lookup_flags &
				~(LOOKUP_FOLLOW | LOOKUP_NO_SYMLINKS),
			    &probe);
			if (slash)
				*slash = saved;
			if (ret) {
				source->error = ret;
				goto out_relative;
			}
			if (d_is_symlink(probe.dentry)) {
				if (lookup_flags & LOOKUP_NO_SYMLINKS) {
					path_put(&probe);
					source->error = -ELOOP;
					goto out_relative;
				}
				if (!(final &&
				      !(lookup_flags & LOOKUP_FOLLOW))) {
					if (lookup_flags & LOOKUP_CACHED) {
						path_put(&probe);
						source->error = -EAGAIN;
						goto out_relative;
					}
					ret = kasumi_read_link_target(
					    &probe, target, sizeof(target));
					if (ret) {
						path_put(&probe);
						if (lookup_flags &
						    LOOKUP_NO_MAGICLINKS) {
							source->error = -ELOOP;
							goto out_relative;
						}
						goto lookup_full;
					}
					if (target[0] == '/' &&
					    !(lookup_flags & LOOKUP_IN_ROOT)) {
						const char *remaining =
						    slash ? slash : "";

						bool found;

						path_put(&probe);
						path_put(&directory);
						found =
						    kasumi_follow_absolute_link(
							target, remaining,
							lookup_flags, source,
							symlink_depth);
						kfree(relative);
						kasumi_identity_domain_put(domain);
						return found;
					}
				}
			}
			path_put(&probe);
			if (!slash)
				break;
			cursor = slash + 1;
		}
	}
lookup_full:
	ret = vfs_path_lookup(directory.dentry, directory.mnt, relative,
			      lookup_flags, &resolved);
	if (ret) {
		source->error = ret;
		goto out_relative;
	}
	path_put(&directory);
	kfree(relative);
	found = kasumi_rule_source_from_path(&resolved, source, domain);
	kasumi_identity_domain_put(domain);
	return found;

out_relative:
	kasumi_identity_domain_put(domain);
	path_put(&directory);
	kfree(relative);
	return false;
}

static bool KASUMI_NOCFI kasumi_rule_get_source_flags_depth(
    const char *pathname, unsigned int lookup_flags,
    struct kasumi_rule_source *source, unsigned int symlink_depth)
{
	u64 generation;
	struct kasumi_entry *entry;
	struct path exact_link = {};
	size_t path_len;
	u32 hash;
	pid_t pid;
	bool found = false;
	bool followed_exact_link = false;

	if (unlikely(!kasumi_config_enabled() || !pathname || !source ||
		     !kasumi_rule_scope_active()))
		return false;
	pid = task_tgid_vnr(current);
	if (atomic_long_read(&kasumi_ioctl_tgid) > 0 &&
	    pid == atomic_long_read(&kasumi_ioctl_tgid))
		return false;
	if (atomic_read(&kasumi_rule_count) == 0)
		return false;
	if (symlink_depth >= KASUMI_SYMLINK_LIMIT) {
		source->error = -ELOOP;
		return false;
	}

	path_len = strlen(pathname);
	if (test_bit(jhash(pathname, (u32)path_len, 0) &
			 (KASUMI_BLOOM_SIZE - 1),
		     kasumi_path_bloom) &&
	    test_bit(jhash(pathname, (u32)path_len, 1) &
			 (KASUMI_BLOOM_SIZE - 1),
		     kasumi_path_bloom)) {
		hash = full_name_hash(NULL, pathname, path_len);
		rcu_read_lock();
		generation = kasumi_rules_generation();
		hlist_for_each_entry_rcu(
		    entry, &kasumi_paths[hash_min(hash, KASUMI_HASH_BITS)],
		    node)
		{
			if (!kasumi_rule_visible(&entry->life, generation))
				continue;
			if (!kasumi_rule_current_sees(entry->rule_flags))
				continue;
			if (entry->src_hash != hash ||
			    strcmp(entry->src, pathname) != 0)
				continue;
			if (entry->source_path_valid &&
			    entry->source_path.dentry &&
			    entry->source_path.mnt) {
				if (!(lookup_flags & LOOKUP_FOLLOW) &&
				    entry->source_nofollow_path_valid) {
					source->path =
					    entry->source_nofollow_path;
					source->stat =
					    entry->source_nofollow_stat;
					source->source_mode =
					    entry->source_nofollow_mode;
					source->visible_ino =
					    entry->nofollow_visible_ino;
					source->visible_dev =
					    entry->nofollow_visible_dev;
					source->stat_valid =
					    entry->source_nofollow_stat_valid;
					source->preserve_visible_metadata =
					    false;
				} else {
					if ((lookup_flags &
					     LOOKUP_NO_SYMLINKS) &&
					    entry->source_nofollow_path_valid) {
						source->error = -ELOOP;
						break;
					}
					source->path = entry->source_path;
					source->stat = entry->visible_stat;
					source->source_mode =
					    entry->source_mode;
					source->visible_ino =
					    entry->visible_ino;
					source->visible_dev =
					    entry->visible_dev;
					source->stat_valid =
					    entry->visible_stat_valid;
					source->preserve_visible_metadata =
					    entry->preserve_visible_metadata;
					if (entry->source_nofollow_path_valid) {
						exact_link =
						    entry->source_nofollow_path;
						path_get(&exact_link);
						followed_exact_link = true;
					}
				}
				source->identity = !(lookup_flags & LOOKUP_FOLLOW) &&
					entry->source_nofollow_path_valid ?
					entry->nofollow_identity : entry->source_identity;
				kasumi_identity_get(source->identity);
				path_get(&source->path);
				found = true;
			}
			break;
		}
		rcu_read_unlock();
	}
	if (found && !kasumi_rule_refresh_exact_source(source)) {
		if (followed_exact_link)
			path_put(&exact_link);
		return false;
	}
	if (found && followed_exact_link) {
		char target[KSM_MAX_LEN_PATHNAME];
		int ret = kasumi_read_link_target(&exact_link, target,
						  sizeof(target));

		path_put(&exact_link);
		if (ret) {
			if (lookup_flags & LOOKUP_NO_MAGICLINKS) {
				kasumi_rule_source_put(source);
				source->error = -ELOOP;
				return false;
			}
			return true;
		}
		if (target[0] == '/' && !(lookup_flags & LOOKUP_IN_ROOT)) {
			struct kasumi_rule_source redirected = {};

			if (kasumi_follow_absolute_link(
				target, "", lookup_flags, &redirected,
				symlink_depth)) {
				kasumi_rule_source_put(source);
				*source = redirected;
				return true;
			}
			if (redirected.error) {
				kasumi_rule_source_put(source);
				source->error = redirected.error;
				return false;
			}
		}
	}
	if (found || source->error)
		return found;
	return kasumi_directory_rule_source(pathname, path_len, lookup_flags,
					    source, symlink_depth);
}

bool KASUMI_NOCFI
kasumi_rule_get_source_flags(const char *pathname, unsigned int lookup_flags,
			     struct kasumi_rule_source *source)
{
	if (source) {
		source->error = 0;
		source->preserve_visible_metadata = false;
	}
	return kasumi_rule_get_source_flags_depth(pathname, lookup_flags,
						  source, 0);
}

/* ---- Slice 4c-v2: pure-virtual directory topology rule-table queries ----- */

static struct rb_root kasumi_rule_prefixes = RB_ROOT;
static DEFINE_RWLOCK(kasumi_rule_prefix_lock);

static DEFINE_HASHTABLE(kasumi_rule_sources, KASUMI_HASH_BITS);

static void kasumi_rule_source_add(struct kasumi_rule_source_index *index,
				   const struct kasumi_entry *entry,
				   const struct path *path,
				   const struct kasumi_inode_identity *identity)
{
	const struct inode *inode = d_inode(path->dentry);
	dev_t dev = inode->i_sb ? inode->i_sb->s_dev : 0;

	index->entry = entry;
	index->inode = inode;
	index->identity = identity;
	write_lock(&kasumi_rule_prefix_lock);
	hash_add_rcu(kasumi_rule_sources, &index->node,
		     (unsigned long)dev ^ inode->i_ino);
	write_unlock(&kasumi_rule_prefix_lock);
}

static void kasumi_rule_source_remove(struct kasumi_entry *entry)
{
	unsigned int i;

	write_lock(&kasumi_rule_prefix_lock);
	for (i = 0; i < ARRAY_SIZE(entry->source_index); i++) {
		if (!hlist_unhashed(&entry->source_index[i].node))
			hlist_del_init_rcu(&entry->source_index[i].node);
	}
	write_unlock(&kasumi_rule_prefix_lock);
}

unsigned long kasumi_rule_source_ino(dev_t dev, u64 ino)
{
	struct kasumi_rule_source_index *index;
	unsigned long result = 0;
	u64 generation;

	rcu_read_lock();
	generation = kasumi_rules_generation();
	hash_for_each_possible_rcu(kasumi_rule_sources, index, node,
				   (unsigned long)dev ^ (unsigned long)ino)
	{
		const struct inode *inode = index->inode;
		const struct kasumi_entry *entry = index->entry;

		if (inode->i_ino != ino ||
		    (inode->i_sb ? inode->i_sb->s_dev : 0) != dev ||
		    !kasumi_rule_visible(&entry->life, generation) ||
		    !kasumi_rule_current_sees(entry->rule_flags))
			continue;
		result = kasumi_identity_ino(index->identity);
		break;
	}
	rcu_read_unlock();
	return result;
}

static struct kasumi_rule_index *kasumi_rule_index_node(struct kasumi_rule_lifetime *life)
{
	if (life->batch_store == KASUMI_BATCH_PATH)
		return &container_of(life, struct kasumi_entry, life)->index;
	if (life->batch_store == KASUMI_BATCH_HIDE)
		return &container_of(life, struct kasumi_hide_entry, life)->index;
	if (life->batch_store == KASUMI_BATCH_MERGE)
		return &container_of(life, struct kasumi_merge_entry, life)->index;
	return NULL;
}

static void kasumi_rule_index_link(struct kasumi_rule_index *index)
{
	struct rb_node **link = &kasumi_rule_prefixes.rb_node, *parent = NULL;

	write_lock(&kasumi_rule_prefix_lock);
	while (*link) {
		struct kasumi_rule_index *other = rb_entry(*link, struct kasumi_rule_index, node);
		int order = strcmp(index->path, other->path);

		parent = *link;
		if (!order)
			order = (unsigned long)index < (unsigned long)other ? -1 : 1;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	rb_link_node(&index->node, parent, link);
	rb_insert_color(&index->node, &kasumi_rule_prefixes);
	index->linked = true;
	write_unlock(&kasumi_rule_prefix_lock);
}
void kasumi_rule_index_add(struct kasumi_rule_lifetime *life)
{
	struct kasumi_rule_index *index = kasumi_rule_index_node(life);

	if (!index || index->linked)
		return;
	index->hide = life->batch_store == KASUMI_BATCH_HIDE;
	if (life->batch_store == KASUMI_BATCH_MERGE)
		index->path = container_of(life, struct kasumi_merge_entry, life)->src;
	else
		index->path = index->hide ?
			container_of(life, struct kasumi_hide_entry, life)->path :
			container_of(life, struct kasumi_entry, life)->src;
	index->life = life;
	kasumi_rule_index_link(index);
	if (life->batch_store == KASUMI_BATCH_PATH) {
		struct kasumi_entry *entry =
		    container_of(life, struct kasumi_entry, life);

		if (entry->source_path_valid)
			kasumi_rule_source_add(&entry->source_index[0], entry,
					       &entry->source_path,
					       entry->source_identity);
		if (entry->source_nofollow_path_valid)
			kasumi_rule_source_add(&entry->source_index[1], entry,
					       &entry->source_nofollow_path,
					       entry->nofollow_identity);
	}
	if (life->batch_store == KASUMI_BATCH_MERGE) {
		struct kasumi_merge_entry *merge = container_of(life, struct kasumi_merge_entry, life);

		if (merge->resolved_src && strcmp(merge->resolved_src, merge->src)) {
			merge->alias_index.path = merge->resolved_src;
			merge->alias_index.life = life;
			kasumi_rule_index_link(&merge->alias_index);
		}
	}
}

static void kasumi_rule_index_unlink(struct kasumi_rule_index *index)
{
	if (!index || !index->linked)
		return;
	write_lock(&kasumi_rule_prefix_lock);
	rb_erase(&index->node, &kasumi_rule_prefixes);
	index->linked = false;
	write_unlock(&kasumi_rule_prefix_lock);
}

void kasumi_rule_index_remove(struct kasumi_rule_lifetime *life)
{
	if (life->batch_store == KASUMI_BATCH_PATH)
		kasumi_rule_source_remove(
		    container_of(life, struct kasumi_entry, life));
	kasumi_rule_index_unlink(kasumi_rule_index_node(life));
	if (life->batch_store == KASUMI_BATCH_MERGE)
		kasumi_rule_index_unlink(&container_of(life, struct kasumi_merge_entry, life)->alias_index);
}

/* Callers retain RCU and the index read lock while traversing. Equal names
 * retain every transaction version until the previous readers have drained. */
static struct kasumi_rule_index *kasumi_rule_prefix_first(const char *path, size_t length)
{
	struct rb_node *node = kasumi_rule_prefixes.rb_node;
	struct kasumi_rule_index *first = NULL;

	while (node) {
		struct kasumi_rule_index *index = rb_entry(node, struct kasumi_rule_index, node);
		int order = strncmp(index->path, path, length);

		if (!order && length != 1)
			order = (unsigned char)index->path[length] - '/';
		if (order < 0) {
			node = node->rb_right;
		} else {
			first = index;
			node = node->rb_left;
		}
	}
	return first;
}

static struct kasumi_rule_index *kasumi_rule_prefix_next(struct kasumi_rule_index *index)
{
	struct rb_node *node = rb_next(&index->node);

	return node ? rb_entry(node, struct kasumi_rule_index, node) : NULL;
}

static struct kasumi_rule_index *kasumi_rule_exact_first(const char *path, size_t length)
{
	struct rb_node *node = kasumi_rule_prefixes.rb_node;
	struct kasumi_rule_index *first = NULL;

	while (node) {
		struct kasumi_rule_index *index = rb_entry(node, struct kasumi_rule_index, node);

		if (strncmp(index->path, path, length) < 0) {
			node = node->rb_right;
		} else {
			first = index;
			node = node->rb_left;
		}
	}
	return first;
}

static bool kasumi_rule_prefix_matches(struct kasumi_rule_index *index,
				       const char *path, size_t length)
{
	return index && !strncmp(index->path, path, length) &&
		(!index->path[length] || length == 1 || index->path[length] == '/');
}

void kasumi_rule_dependencies(const char *path, u64 generation,
	void (*visit)(struct kasumi_rule_lifetime *, void *), void *data)
{
	struct kasumi_rule_index *index;
	size_t length, prefix;

	if (!path || path[0] != '/')
		return;
	length = strlen(path);
	rcu_read_lock();
	read_lock(&kasumi_rule_prefix_lock);
	for (prefix = 1; prefix <= length; prefix++) {
		if (prefix != 1 && prefix != length && path[prefix] != '/')
			continue;
		index = kasumi_rule_exact_first(path, prefix);
		while (index && !strncmp(index->path, path, prefix) && !index->path[prefix]) {
			if (kasumi_rule_visible(index->life, generation))
				visit(index->life, data);
			index = kasumi_rule_prefix_next(index);
		}
	}
	for (index = kasumi_rule_prefix_first(path, length);
	     kasumi_rule_prefix_matches(index, path, length);
	     index = kasumi_rule_prefix_next(index)) {
		if (kasumi_rule_visible(index->life, generation))
			visit(index->life, data);
	}
	read_unlock(&kasumi_rule_prefix_lock);
	rcu_read_unlock();
}

int kasumi_rule_vpath_collect(struct file *file, struct list_head *out,
			      u64 *generation, void *data)
{
	const char *dir = data;
	struct kasumi_entry *rule;
	struct kasumi_hide_entry *hide;
	struct kasumi_rule_index *index;
	struct kasumi_dir_entry *item;
	DECLARE_HASHTABLE(children, 6);
	size_t dir_len;
	int error = 0;

	(void)file;
	if (!dir)
		return -EINVAL;
	dir_len = strlen(dir);
	hash_init(children);
	rcu_read_lock();
	*generation = kasumi_rules_generation();
	read_lock(&kasumi_rule_prefix_lock);
	for (index = kasumi_rule_prefix_first(dir, dir_len);
	     kasumi_rule_prefix_matches(index, dir, dir_len);
	     index = kasumi_rule_prefix_next(index)) {
		const char *name, *slash;
		struct kasumi_dir_entry *existing;
		size_t name_len, prefix_len;
		u64 ino;
		u32 hash;
		u8 type;
		bool leaf;
		struct kasumi_inode_identity *identity = NULL;

		if (index->life->batch_store != KASUMI_BATCH_PATH)
			continue;
		rule = container_of(index, struct kasumi_entry, index);
		if (!kasumi_rule_visible(&rule->life, *generation) ||
		    strncmp(rule->src, dir, dir_len))
			continue;
		prefix_len = dir_len;
		if (dir_len != 1 || dir[0] != '/') {
			if (rule->src[prefix_len] != '/')
				continue;
			prefix_len++;
		}
		name = rule->src + prefix_len;
		if (!*name)
			continue;
		slash = strchr(name, '/');
		name_len = slash ? slash - name : strlen(name);
		leaf = slash == NULL;
		if (leaf) {
			umode_t mode = rule->source_nofollow_path_valid ?
				rule->source_nofollow_mode : rule->source_mode;

			identity = rule->source_nofollow_path_valid ?
				rule->nofollow_identity : rule->source_identity;
			ino = rule->source_nofollow_path_valid ?
				rule->nofollow_visible_ino : rule->visible_ino;
			type = (mode & S_IFMT) >> 12;
		} else {
			ino = 0;
			type = DT_DIR;
		}
		hash = full_name_hash(NULL, name, name_len);
		item = NULL;
		hash_for_each_possible(children, existing, name_node, hash) {
			if (existing->name_len == name_len &&
			    !memcmp(existing->name, name, name_len)) {
				item = existing;
				break;
			}
		}
		if (!item) {
			item = kasumi_dir_entry_alloc(name, name_len, GFP_ATOMIC);
			if (!item) {
				error = -ENOMEM;
				goto out;
			}
			item->redirect = true;
			list_add_tail(&item->list, out);
			hash_add(children, &item->name_node, hash);
		}
		if ((!item->direct_rule && leaf) ||
		    (item->direct_rule == leaf && rule->sequence >= item->sequence)) {
			kasumi_identity_put(item->identity);
			item->identity = identity;
			kasumi_identity_get(identity);
			item->ino = ino;
			item->type = type;
			item->direct_rule = leaf;
			item->sequence = rule->sequence;
			item->rule_flags = leaf ? rule->rule_flags : 0;
		}
		if ((rule->rule_flags & KSM_RULE_BYPASS_POLICY) &&
		    (!item->bypass_redirect || (!item->bypass_direct_rule && leaf) ||
		     (item->bypass_direct_rule == leaf &&
		      rule->sequence >= item->bypass_sequence))) {
			kasumi_identity_put(item->bypass_identity);
			item->bypass_identity = identity;
			kasumi_identity_get(identity);
			item->bypass_redirect = true;
			item->bypass_direct_rule = leaf;
			item->bypass_sequence = rule->sequence;
			item->bypass_ino = ino;
			item->bypass_type = type;
		}
	}
	for (index = kasumi_rule_prefix_first(dir, dir_len);
	     kasumi_rule_prefix_matches(index, dir, dir_len);
	     index = kasumi_rule_prefix_next(index)) {
		const char *name;
		struct kasumi_dir_entry *existing;
		size_t prefix_len = dir_len, name_len;
		u32 hash;

		if (!index->hide)
			continue;
		hide = container_of(index, struct kasumi_hide_entry, index);
		if (!kasumi_rule_visible(&hide->life, *generation) ||
		    strncmp(hide->path, dir, dir_len))
			continue;
		if (dir_len != 1 || dir[0] != '/') {
			if (hide->path[prefix_len] != '/')
				continue;
			prefix_len++;
		}
		name = hide->path + prefix_len;
		name_len = strlen(name);
		if (!name_len || strchr(name, '/'))
			continue;
		hash = full_name_hash(NULL, name, name_len);
		item = NULL;
		hash_for_each_possible(children, existing, name_node, hash) {
			if (existing->name_len == name_len &&
			    !memcmp(existing->name, name, name_len)) {
				item = existing;
				break;
			}
		}
		if (!item) {
			item = kasumi_dir_entry_alloc(name, name_len, GFP_ATOMIC);
			if (!item) {
				error = -ENOMEM;
				goto out;
			}
			list_add_tail(&item->list, out);
			hash_add(children, &item->name_node, hash);
		}
		if (!item->hide || hide->sequence >= item->hide_sequence) {
			item->hide = true;
			item->hide_sequence = hide->sequence;
			item->hide_flags = hide->rule_flags;
			item->hide_storage_managed = hide->storage_managed;
		}
	}
	out:
	read_unlock(&kasumi_rule_prefix_lock);
	rcu_read_unlock();
	list_for_each_entry(item, out, list) {
		struct kasumi_inode_identity *identity;
		char *path;

		hash_del(&item->name_node);
		if (error || (!item->redirect && !item->bypass_redirect) ||
		    (item->direct_rule &&
		    (!item->bypass_redirect || item->bypass_direct_rule)))
			continue;
		path = kasprintf(GFP_KERNEL, "%s%s%s", dir,
			strcmp(dir, "/") ? "/" : "", item->name);
		if (!path) {
			error = -ENOMEM;
			continue;
		}
		error = kasumi_identity_path_get(path, &identity);
		kfree(path);
		if (error)
			continue;
		if (!item->direct_rule) {
			item->identity = identity;
			kasumi_identity_get(identity);
			item->ino = kasumi_identity_ino(identity);
		}
		if (item->bypass_redirect && !item->bypass_direct_rule) {
			item->bypass_identity = identity;
			kasumi_identity_get(identity);
			item->bypass_ino = kasumi_identity_ino(identity);
		}
		kasumi_identity_put(identity);
	}
	return error;
}

bool kasumi_rule_directory_overridden(const char *path, u64 generation)
{
	struct kasumi_rule_index *index;
	size_t length, prefix;
	bool found = false;

	if (!path || path[0] != '/')
		return true;
	length = strlen(path);
	rcu_read_lock();
	read_lock(&kasumi_rule_prefix_lock);
	for (index = kasumi_rule_prefix_first(path, length);
	     kasumi_rule_prefix_matches(index, path, length);
	     index = kasumi_rule_prefix_next(index)) {
		if (kasumi_rule_visible(index->life, generation)) {
			found = true;
			break;
		}
	}
	for (prefix = 1; !found && prefix <= length; prefix++) {
		if (prefix != 1 && prefix != length && path[prefix] != '/')
			continue;
		for (index = kasumi_rule_exact_first(path, prefix);
		     index && !strncmp(index->path, path, prefix) && !index->path[prefix];
		     index = kasumi_rule_prefix_next(index)) {
			if (index->life->batch_store == KASUMI_BATCH_MERGE &&
			    kasumi_rule_visible(index->life, generation)) {
				found = true;
				break;
			}
		}
	}
	read_unlock(&kasumi_rule_prefix_lock);
	rcu_read_unlock();
	return found;
}

bool kasumi_rule_directory_has_injects(const char *path, u64 generation)
{
	struct kasumi_rule_index *index;
	size_t length = strlen(path);
	bool found = false;

	rcu_read_lock();
	read_lock(&kasumi_rule_prefix_lock);
	for (index = kasumi_rule_prefix_first(path, length);
	     kasumi_rule_prefix_matches(index, path, length);
	     index = kasumi_rule_prefix_next(index)) {
		const char *child = index->path + length + (length != 1);

		if (index->life->batch_store == KASUMI_BATCH_PATH &&
		    kasumi_rule_visible(index->life, generation) &&
		    *child && !strchr(child, '/')) {
			found = true;
			break;
		}
	}
	for (index = kasumi_rule_exact_first(path, length);
	     !found && index && !strcmp(index->path, path);
	     index = kasumi_rule_prefix_next(index)) {
		if (index->life->batch_store == KASUMI_BATCH_MERGE &&
		    kasumi_rule_visible(index->life, generation))
			found = true;
	}
	read_unlock(&kasumi_rule_prefix_lock);
	rcu_read_unlock();
	return found;
}

bool kasumi_rule_vpath_flags(const char *path, u64 generation, u32 *flags)
{
	struct kasumi_entry *entry;
	struct kasumi_rule_index *index;
	size_t len = strlen(path);
	bool found = false;

	*flags = 0;
	read_lock(&kasumi_rule_prefix_lock);
	for (index = kasumi_rule_prefix_first(path, len);
	     kasumi_rule_prefix_matches(index, path, len);
	     index = kasumi_rule_prefix_next(index)) {
		if (index->life->batch_store != KASUMI_BATCH_PATH)
			continue;
		entry = container_of(index, struct kasumi_entry, index);
		if (!kasumi_rule_visible(&entry->life, generation) ||
		    strncmp(entry->src, path, len) || entry->src[len] != '/')
			continue;
		*flags |= entry->rule_flags & KSM_RULE_BYPASS_POLICY;
		found = true;
	}
	read_unlock(&kasumi_rule_prefix_lock);
	return found;
}

/* A projected parent is enforced by vnode lookup and iteration. Installing its
 * hide on the source directory would affect unrelated aliases.
 */
struct kasumi_projected_parent_selection {
	const char *parent;
	size_t length;
	struct kasumi_entry *best;
	size_t best_length;
	bool virtual_prefix;
};

static void kasumi_projected_parent_visit(struct kasumi_rule_lifetime *life, void *data)
{
	struct kasumi_projected_parent_selection *selection = data;
	struct kasumi_entry *entry;
	size_t rule_length;

	if (life->batch_store != KASUMI_BATCH_PATH)
		return;
	entry = container_of(life, struct kasumi_entry, life);
	rule_length = strlen(entry->src);
	if (rule_length > selection->length &&
	    !strncmp(entry->src, selection->parent, selection->length) &&
	    (selection->length == 1 || entry->src[selection->length] == '/'))
		selection->virtual_prefix = true;
	if (rule_length > selection->length ||
	    strncmp(entry->src, selection->parent, rule_length) ||
	    (rule_length != 1 && rule_length != selection->length &&
	     selection->parent[rule_length] != '/'))
		return;
	if (!selection->best || rule_length > selection->best_length ||
	    (rule_length == selection->best_length && entry->sequence > selection->best->sequence)) {
		selection->best = entry;
		selection->best_length = rule_length;
	}
}

int KASUMI_NOCFI kasumi_rule_projected_parent(const char *visible, u64 generation,
					    struct path *source_parent)
{
	struct kasumi_internal_vfs_guard guard;
	struct kasumi_entry *best;
	struct kasumi_projected_parent_selection selection = {};
	struct path directory = {}, resolved = {};
	char *parent, *slash;
	size_t length, best_length = 0;
	bool virtual_prefix = false;
	int ret = 0;

	memset(source_parent, 0, sizeof(*source_parent));
	if (!visible || visible[0] != '/')
		return -EINVAL;
	parent = kstrdup(visible, GFP_KERNEL);
	if (!parent)
		return -ENOMEM;
	slash = strrchr(parent, '/');
	if (!slash[1]) {
		ret = -EINVAL;
		goto out;
	}
	if (slash == parent)
		slash[1] = '\0';
	else
		*slash = '\0';
	length = strlen(parent);
	rcu_read_lock();
	selection.parent = parent;
	selection.length = length;
	kasumi_rule_dependencies(parent, generation, kasumi_projected_parent_visit, &selection);
	best = selection.best;
	best_length = selection.best_length;
	virtual_prefix = selection.virtual_prefix;
	if (best) {
		if (!best->source_path_valid || !S_ISDIR(best->source_mode)) {
			ret = -ENOTDIR;
		} else {
			directory = best->source_path;
			path_get(&directory);
		}
	}
	rcu_read_unlock();
	if (ret)
		goto out;
	if (directory.dentry && best_length == length) {
		*source_parent = directory;
		ret = 1;
		goto out;
	}
	if (!directory.dentry && !virtual_prefix)
		goto out;
	kasumi_vfs_internal_enter(&guard);
	if (directory.dentry) {
		const char *relative = parent + best_length;

		if (*relative == '/')
			relative++;
		ret = vfs_path_lookup(directory.dentry, directory.mnt, relative,
			LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &resolved);
		path_put(&directory);
		if (!ret) {
			*source_parent = resolved;
			ret = 1;
		} else if (ret == -ENOENT && virtual_prefix) {
			ret = 1;
		}
	} else {
		ret = kern_path(parent, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &resolved);
		if (!ret)
			path_put(&resolved);
		else if (ret == -ENOENT)
			ret = 1;
	}
	kasumi_vfs_internal_leave(&guard);
out:
	kfree(parent);
	return ret;
}

bool kasumi_rule_vpath_visible(const char *path)
{
	u32 flags;
	bool visible;

	if (!path || !kasumi_rule_scope_active())
		return false;
	if (kasumi_policy_current_is_view_target())
		return true;
	rcu_read_lock();
	visible = kasumi_rule_vpath_flags(path, kasumi_rules_generation(), &flags) &&
		  (flags & KSM_RULE_BYPASS_POLICY);
	rcu_read_unlock();
	return visible;
}

static int KASUMI_NOCFI kasumi_rule_vpath_child_exact(const char *dir, const char *child,
					 struct path *leaf_src,
					 umode_t *leaf_mode,
					 unsigned long *leaf_ino,
					 struct kasumi_inode_identity **identity)
{
	u64 generation, sequence = 0;
	struct kasumi_hide_entry *hide;
	struct kasumi_entry *entry;
	char *full;
	size_t dlen, clen, full_len;
	int kind = KASUMI_VPATH_NONE;
	int bkt;

	if (!dir || !child || !*child)
		return KASUMI_VPATH_NONE;
	dlen = strlen(dir);
	clen = strlen(child);
	full = kmalloc(dlen + 1 + clen + 1, GFP_KERNEL);
	if (!full)
		return -ENOMEM;
	memcpy(full, dir, dlen);
	full[dlen] = '/';
	memcpy(full + dlen + 1, child, clen);
	full[dlen + 1 + clen] = '\0';
	full_len = dlen + 1 + clen;

	rcu_read_lock();
	generation = kasumi_rules_generation();
	/* Exact rule at dir/child -> a leaf redirect. */
	hash_for_each_rcu(kasumi_paths, bkt, entry, node)
	{
		if (!kasumi_rule_visible(&entry->life, generation))
			continue;
		if (!entry->src || strcmp(entry->src, full))
			continue;
		sequence = entry->sequence;
		if (!kasumi_rule_current_sees(entry->rule_flags) ||
		    (!entry->source_path_valid && !entry->source_nofollow_path_valid))
			continue;
		if (entry->source_nofollow_path_valid) {
			if (identity) {
				*identity = entry->nofollow_identity;
				kasumi_identity_get(*identity);
			}
			if (leaf_src) {
				*leaf_src = entry->source_nofollow_path;
				path_get(leaf_src);
			}
			if (leaf_mode)
				*leaf_mode = entry->source_nofollow_mode;
			if (leaf_ino)
				*leaf_ino = entry->nofollow_visible_ino;
		} else {
			if (identity) {
				*identity = entry->source_identity;
				kasumi_identity_get(*identity);
			}
			if (leaf_src) {
				*leaf_src = entry->source_path;
				path_get(leaf_src);
			}
			if (leaf_mode)
				*leaf_mode = entry->source_mode;
			if (leaf_ino)
				*leaf_ino = entry->visible_ino;
		}
		kind = KASUMI_VPATH_LEAF;
		break;
	}
	hash_for_each_rcu(kasumi_hide_paths, bkt, hide, node) {
		if (kasumi_rule_visible(&hide->life, generation) &&
		    hide->sequence >= sequence && !strcmp(hide->path, full) &&
		    kasumi_hide_scope_allowed(hide->storage_managed, hide->rule_flags)) {
			kind = KASUMI_VPATH_HIDDEN;
			break;
		}
	}
	/* Otherwise, a prefix of some rule -> a deeper virtual directory. */
	if (kind == KASUMI_VPATH_NONE) {
		hash_for_each_rcu(kasumi_paths, bkt, entry, node)
		{
			if (!kasumi_rule_visible(&entry->life, generation))
				continue;
			if (!kasumi_rule_current_sees(entry->rule_flags))
				continue;
			if (!entry->src)
				continue;
			if (strncmp(entry->src, full, full_len) == 0 &&
			    entry->src[full_len] == '/') {
				kind = KASUMI_VPATH_VDIR;
				break;
			}
		}
	}
	rcu_read_unlock();
	if (kind == KASUMI_VPATH_HIDDEN) {
		if (leaf_src && leaf_src->dentry) {
			path_put(leaf_src);
			memset(leaf_src, 0, sizeof(*leaf_src));
		}
		if (identity) {
			kasumi_identity_put(*identity);
			*identity = NULL;
		}
	}
	kfree(full);
	return kind;
}

int kasumi_rule_vpath_child(const char *dir, const char *child,
	struct path *leaf_src, umode_t *leaf_mode, unsigned long *leaf_ino,
	struct kasumi_inode_identity **identity, struct dentry *parent)
{
	LIST_HEAD(entries);
	struct kasumi_dir_entry *entry, *best, *hidden;
	u64 generation;
	u64 best_sequence, exact_sequence;
	u32 query_hash;
	bool view, best_direct;
	int kind;

	if (!parent || !(READ_ONCE(parent->d_flags) & DCACHE_OP_COMPARE))
		return kasumi_rule_vpath_child_exact(dir, child, leaf_src,
			leaf_mode, leaf_ino, identity);
	if (!kasumi_rule_scope_active())
		return KASUMI_VPATH_NONE;
retry:
	view = kasumi_policy_current_is_view_target();
	best = NULL;
	hidden = NULL;
	exact_sequence = 0;
	best_direct = false;
	best_sequence = 0;
	kind = kasumi_dir_name_hash(parent, child, strlen(child), &query_hash);
	if (kind)
		return kind;
	kind = kasumi_rule_vpath_collect(NULL, &entries, &generation, (void *)dir);
	if (kind)
		goto out;
	kind = KASUMI_VPATH_NONE;
	list_for_each_entry(entry, &entries, list) {
		struct dentry *candidate;
		u64 sequence = view ? entry->sequence : entry->bypass_sequence;
		u32 hash;
		bool direct = view ? entry->direct_rule : entry->bypass_direct_rule;
		bool equal;

		kind = kasumi_dir_name_hash(parent, entry->name, entry->name_len, &hash);
		if (kind)
			goto out;
		if (hash != query_hash)
			continue;
		candidate = kasumi_dir_name_probe(parent, entry->name, entry->name_len);
		if (IS_ERR(candidate)) {
			kind = PTR_ERR(candidate);
			goto out;
		}
		equal = kasumi_dir_name_equal(parent, candidate, entry->name,
			entry->name_len, child, strlen(child));
		dput(candidate);
		if (!equal)
			continue;
		if (entry->direct_rule && entry->sequence > exact_sequence)
			exact_sequence = entry->sequence;
		if (entry->hide && (!hidden || entry->hide_sequence >= hidden->hide_sequence))
			hidden = entry;
		if (!entry->redirect || (!kasumi_rule_current_sees(entry->rule_flags) &&
		    !entry->bypass_redirect))
			continue;
		if (!best || (!best_direct && direct) ||
		    (best_direct == direct && sequence >= best_sequence)) {
			best = entry;
			best_direct = direct;
			best_sequence = sequence;
		}
	}
	if (hidden && hidden->hide_sequence >= exact_sequence &&
	    kasumi_hide_scope_allowed(hidden->hide_storage_managed, hidden->hide_flags))
		kind = KASUMI_VPATH_HIDDEN;
	else
		kind = best ? kasumi_rule_vpath_child_exact(dir, best->name, leaf_src,
			leaf_mode, leaf_ino, identity) : KASUMI_VPATH_NONE;
	if (generation != kasumi_rules_generation()) {
		if (leaf_src && leaf_src->dentry) {
			path_put(leaf_src);
			memset(leaf_src, 0, sizeof(*leaf_src));
		}
		if (identity) {
			kasumi_identity_put(*identity);
			*identity = NULL;
		}
		kasumi_dir_entries_free(&entries);
		cond_resched();
		goto retry;
	}
out:
	kasumi_dir_entries_free(&entries);
	return kind;
}


/* ======================================================================
 * Part 14: Hide Logic
 * ====================================================================== */

static bool kasumi_hide_rule_matches(const char *pathname)
{
	u64 generation;
	struct kasumi_hide_entry *he;
	u32 hash;
	size_t len;

	if (!pathname || !*pathname)
		return false;
	if (atomic_read(&kasumi_hide_count) == 0)
		return false;

	len = strlen(pathname);
	{
		unsigned long bh1 =
		    jhash(pathname, (u32)len, 0) & (KASUMI_BLOOM_SIZE - 1);
		unsigned long bh2 =
		    jhash(pathname, (u32)len, 1) & (KASUMI_BLOOM_SIZE - 1);

		if (!test_bit(bh1, kasumi_hide_bloom) ||
		    !test_bit(bh2, kasumi_hide_bloom))
			return false;
	}

	hash = full_name_hash(NULL, pathname, len);
	rcu_read_lock();
	generation = kasumi_rules_generation();
	hlist_for_each_entry_rcu(
	    he, &kasumi_hide_paths[hash_min(hash, KASUMI_HASH_BITS)], node)
	{
		if (!kasumi_rule_visible(&he->life, generation))
			continue;
		if (he->path_hash == hash && strcmp(he->path, pathname) == 0) {
			bool hidden = kasumi_hide_scope_allowed(
			    READ_ONCE(he->storage_managed),
			    READ_ONCE(he->rule_flags));

			rcu_read_unlock();
			return hidden;
		}
	}
	rcu_read_unlock();
	return false;
}

int kasumi_rule_child_mutation(struct inode *parent, const char *visible_path,
			       const struct qstr *name)
{
	const struct kasumi_vnode_info *info =
		kasumi_vnode_is_ours(parent) ? parent->i_private : NULL;
	char *child;
	u64 generation;
	int kind, ret;

	if (!name || !name->len || name->len > NAME_MAX)
		return -EINVAL;
	child = kstrndup(name->name, name->len, GFP_KERNEL);
	if (!child)
		return -ENOMEM;
	do {
		generation = kasumi_rules_generation();
		ret = 0;
		if (kasumi_dirhijack_hidden(parent, child, name->len)) {
			ret = -ENOENT;
		} else {
			kind = kasumi_dirhijack_child_visible(parent, child, name->len);
			if (kind > 0)
				ret = -EXDEV;
			else if (kind < 0 && visible_path) {
				kind = kasumi_rule_vpath_child(visible_path, child,
					NULL, NULL, NULL, NULL,
					info ? info->source.dentry : NULL);
				ret = kind < 0 ? kind : kind != KASUMI_VPATH_NONE ?
					-EXDEV : 0;
			}
		}
	} while (generation != kasumi_rules_generation());
	kfree(child);
	return ret;
}

bool kasumi_should_hide(const char *pathname)
{
	if (!kasumi_rule_scope_active())
		return false;
	return kasumi_hide_rule_matches(pathname);
}
