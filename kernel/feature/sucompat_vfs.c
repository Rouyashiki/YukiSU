#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/security.h>
#include <linux/string.h>
#include <linux/version.h>

#include "feature/sucompat_exec.h"
#include "feature/sucompat_prompt.h"
#include "feature/sucompat_vfs.h"
#include "kasumi_bootstrap.h"
#include "kasumi_dirhijack.h"
#include "kasumi_vnode.h"
#include "ksu.h"
#include "policy/allowlist.h"
#include "selinux/selinux.h"

static DEFINE_MUTEX(su_view_lock);
static bool su_ready;
static bool su_enabled;
static bool su_prompt_enabled;
static char su_path[KSU_SU_PATH_MAX] = KSU_SU_PATH_DEFAULT;
static struct path su_parent;
static char su_name[NAME_MAX + 1];
static unsigned long su_ino;
static unsigned long su_next_ino = (1UL << 63) | 0x53550000UL;
static const struct inode_operations su_inode_ops;

bool ksu_sucompat_vfs_enabled(void)
{
	return kasumi_is_ready() && READ_ONCE(su_enabled);
}
bool ksu_sucompat_vfs_active(void)
{
	return ksu_sucompat_vfs_enabled();
}

bool ksu_sucompat_vfs_current_ino(unsigned long ino)
{
	return ksu_sucompat_vfs_enabled() && ino && ino == READ_ONCE(su_ino);
}
bool ksu_sucompat_vfs_prompt_enabled(void)
{
	return READ_ONCE(su_prompt_enabled);
}

bool ksu_sucompat_vfs_prompt_visible(void)
{
	uid_t uid = current_uid().val;

#ifdef CONFIG_COMPAT
	if (is_compat_task())
		return false;
#endif
	return ksu_sucompat_vfs_enabled() && READ_ONCE(su_prompt_enabled) &&
	       ksu_sucompat_prompt_consumer_ready() && is_appuid(uid) &&
	       !is_isolated_process(uid) && !ksu_uid_should_umount(uid) &&
	       !ksu_is_allow_uid_for_current(uid);
}

bool ksu_sucompat_vfs_visible(void)
{
#ifdef CONFIG_COMPAT
	if (is_compat_task())
		return false;
#endif
	return ksu_sucompat_vfs_enabled() &&
	       (ksu_is_allow_uid_for_current(current_uid().val) ||
		ksu_sucompat_vfs_prompt_visible());
}

bool ksu_sucompat_vfs_is_inode(const struct inode *inode)
{
	return inode && inode->i_op == &su_inode_ops;
}

unsigned long ksu_sucompat_vfs_path_ino(const struct path *path)
{
	const struct inode *inode;

	if (!path || !path->dentry)
		return 0;
	inode = d_inode(path->dentry);
	if (!inode)
		return 0;
	/* Module overlays can wrap an earlier su binding. */
	if (!ksu_sucompat_vfs_is_inode(inode))
		inode = d_real_inode(path->dentry);
	return ksu_sucompat_vfs_is_inode(inode) ? inode->i_ino : 0;
}

bool ksu_sucompat_vfs_is_file(const struct file *file)
{
	return file && ksu_sucompat_vfs_is_inode(file_inode(file));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int su_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int su_permission(struct user_namespace *userns, struct inode *inode,
			 int mask)
#else
static int su_permission(struct inode *inode, int mask)
#endif
{
	if (!ksu_sucompat_vfs_visible() ||
	    !ksu_sucompat_vfs_current_ino(inode->i_ino))
		return -EACCES;
	return mask & MAY_WRITE ? -EROFS : 0;
}

static int su_open(struct inode *inode, struct file *file)
{
	return ksu_sucompat_exec_file_open(file);
}

static const struct inode_operations su_inode_ops = {
    .permission = su_permission,
};
static const struct file_operations su_file_ops = {
    .owner = THIS_MODULE,
    .open = su_open,
    .release = ksu_sucompat_exec_file_release,
};

int ksu_sucompat_vfs_setup_inode(struct inode *inode)
{
	int ret;

	inode->i_op = &su_inode_ops;
	inode->i_fop = &su_file_ops;
	inode->i_mode = S_IFREG | 0555;
	inode_lock(inode);
	ret = security_inode_notifysecctx(inode, (void *)KSU_FILE_CONTEXT,
					  sizeof(KSU_FILE_CONTEXT) - 1);
	inode_unlock(inode);
	return ret;
}

static int su_bind_locked(const char *path)
{
	const struct cred *old;
	struct path parent = {};
	unsigned long ino;
	int ret;

	lockdep_assert_held(&su_view_lock);
	if (!ksu_cred)
		return -EAGAIN;
	old = override_creds(ksu_cred);
	if (su_enabled && !strcmp(path, su_path)) {
		ret = kasumi_dirhijack_match_su(path, &su_parent,
						READ_ONCE(su_ino));
		if (ret) {
			if (ret > 0)
				ret = 0;
			goto out;
		}
	}
	if (su_next_ino == ULONG_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	ino = ++su_next_ino;
	ret = kasumi_dirhijack_add_su(path, ino, &parent);
	if (!ret) {
		unsigned long previous = READ_ONCE(su_ino);

		/* Publish one binding; old dentries and in-flight execs lose
		 * admission. */
		WRITE_ONCE(su_ino, ino);
		if (su_parent.dentry) {
			kasumi_dirhijack_del_su(&su_parent, su_name, previous);
			path_put(&su_parent);
		}
		su_parent = parent;
		strscpy(su_name, strrchr(path, '/') + 1, sizeof(su_name));
	}
out:
	revert_creds(old);
	return ret;
}

static int su_validate_path(const char *path)
{
	const char *component;
	size_t len = strnlen(path, KSU_SU_PATH_MAX);
	size_t i;

	if (len == KSU_SU_PATH_MAX)
		return -ENAMETOOLONG;
	if (len < 2 || path[0] != '/')
		return -EINVAL;
	component = path + 1;
	for (i = 1; i <= len; i++) {
		size_t size;

		if (path[i] && (unsigned char)path[i] < 0x20)
			return -EINVAL;
		if (path[i] && path[i] != '/')
			continue;
		size = path + i - component;
		if (!size || (size == 1 && component[0] == '.') ||
		    (size == 2 && component[0] == '.' && component[1] == '.'))
			return -EINVAL;
		if (size > NAME_MAX)
			return -ENAMETOOLONG;
		component = path + i + 1;
	}
	return 0;
}

int ksu_sucompat_vfs_get_config(struct ksu_su_path_config *config)
{
	memset(config, 0, sizeof(*config));
	config->version = KSU_SU_PATH_VERSION;
	config->size = sizeof(*config);
	mutex_lock(&su_view_lock);
	config->flags = ksu_sucompat_vfs_enabled() ? KSU_SU_PATH_ENABLED : 0;
	strscpy(config->path, su_path, sizeof(config->path));
	mutex_unlock(&su_view_lock);
	return 0;
}

bool ksu_sucompat_vfs_reserved_path(const char *path)
{
	bool reserved;

	mutex_lock(&su_view_lock);
	reserved = !strcmp(path, su_path);
	mutex_unlock(&su_view_lock);
	return reserved;
}

int ksu_sucompat_vfs_set_config(const struct ksu_su_path_config *config)
{
	int ret;

	if (config->version != KSU_SU_PATH_VERSION ||
	    config->size != sizeof(*config) || config->flags ||
	    config->reserved)
		return -EINVAL;
	ret = su_validate_path(config->path);
	if (ret)
		return ret;
	mutex_lock(&su_view_lock);
	if (strcmp(su_path, config->path)) {
		if (su_enabled) {
			ret = su_bind_locked(config->path);
			if (ret)
				goto out;
		} else if (su_parent.dentry) {
			kasumi_dirhijack_del_su(&su_parent, su_name,
						READ_ONCE(su_ino));
			path_put(&su_parent);
			memset(&su_parent, 0, sizeof(su_parent));
			WRITE_ONCE(su_ino, 0);
		}
		strscpy(su_path, config->path, sizeof(su_path));
	}
out:
	mutex_unlock(&su_view_lock);
	return ret;
}

int ksu_sucompat_vfs_refresh(void)
{
	int ret = 0;

	mutex_lock(&su_view_lock);
	if (su_ready && su_enabled)
		ret = su_bind_locked(su_path);
	mutex_unlock(&su_view_lock);
	return ret;
}

int ksu_sucompat_vfs_set_enabled(bool enabled)
{
	int ret = 0;

	mutex_lock(&su_view_lock);
	if (enabled && (!su_ready || !kasumi_is_ready())) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (enabled) {
		ret = su_bind_locked(su_path);
		if (ret)
			goto out;
	}
	WRITE_ONCE(su_enabled, enabled);
out:
	mutex_unlock(&su_view_lock);
	return ret;
}

int ksu_sucompat_vfs_set_prompt_enabled(bool enabled)
{
	if (enabled && (!READ_ONCE(su_ready) || !kasumi_is_ready()))
		return -EOPNOTSUPP;
	WRITE_ONCE(su_prompt_enabled, enabled);
	return 0;
}

int ksu_sucompat_vfs_init(void)
{
	WRITE_ONCE(su_ready, true);
	return 0;
}

void ksu_sucompat_vfs_exit(void)
{
	mutex_lock(&su_view_lock);
	WRITE_ONCE(su_enabled, false);
	WRITE_ONCE(su_prompt_enabled, false);
	if (su_parent.dentry) {
		kasumi_dirhijack_del_su(&su_parent, su_name, READ_ONCE(su_ino));
		path_put(&su_parent);
		memset(&su_parent, 0, sizeof(su_parent));
	}
	if (su_ready) {
		WRITE_ONCE(su_ready, false);
	}
	mutex_unlock(&su_view_lock);
}
