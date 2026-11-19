#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "fixed_policy.h"

#include "ksu.h"
#define yz_policy_open(path)                                                   \
	filp_open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0)
#define yz_policy_read kernel_read
#define yz_policy_close(file) filp_close(file, NULL)
#define YZ_POLICY_PRIMARY "/metadata/watchdog/ksu/yukizygisk_policy.bin"
#define YZ_POLICY_SECONDARY "/metadata/ksu/yukizygisk_policy.bin"

static DEFINE_MUTEX(yz_fixed_lock);
static DEFINE_SPINLOCK(yz_fixed_missing_lock);
static struct yz_fixed_policy_config yz_fixed_config;
static struct yz_fixed_policy_status yz_fixed_status = {
    .version = YZ_FIXED_POLICY_VERSION,
};
static bool yz_fixed_latched;
static bool yz_fixed_apply_pending;

static int yz_fixed_read_config(void)
{
	const struct cred *saved;
	struct file *file;
	struct inode *inode;
	loff_t pos = 0;
	ssize_t size;
	u32 i;
	int ret = 0;

	saved = ksu_cred ? override_creds(ksu_cred) : NULL;

	file = yz_policy_open(YZ_POLICY_PRIMARY);
#ifdef YZ_POLICY_SECONDARY
	if (IS_ERR(file) && PTR_ERR(file) == -ENOENT)
		file = yz_policy_open(YZ_POLICY_SECONDARY);
#endif
	if (IS_ERR(file)) {
		ret = PTR_ERR(file) == -ENOENT ? 0 : PTR_ERR(file);
		goto out;
	}
	inode = file_inode(file);
	if (!S_ISREG(inode->i_mode) || !uid_eq(inode->i_uid, GLOBAL_ROOT_UID) ||
	    (inode->i_mode & 0022) || READ_ONCE(inode->i_nlink) != 1 ||
	    i_size_read(inode) != sizeof(yz_fixed_config)) {
		ret = -EINVAL;
		goto close;
	}
	size = yz_policy_read(file, &yz_fixed_config, sizeof(yz_fixed_config),
			      &pos);
	if (size != sizeof(yz_fixed_config)) {
		ret = size < 0 ? size : -EIO;
		goto close;
	}
	if (READ_ONCE(inode->i_nlink) != 1 ||
	    yz_fixed_config.version != YZ_FIXED_POLICY_VERSION ||
	    yz_fixed_config.enabled > 1 || yz_fixed_config.reserved ||
	    yz_fixed_config.count > YZ_FIXED_DOMAIN_MAX) {
		ret = -EINVAL;
		goto close;
	}
	for (i = 0; i < yz_fixed_config.count; i++) {
		const char *name = yz_fixed_config.domains[i];
		size_t len = strnlen(name, YZ_FIXED_NAME_MAX);
		size_t j;

		if (!len || len == YZ_FIXED_NAME_MAX || !strcmp(name, "self")) {
			ret = -EINVAL;
			goto close;
		}
		for (j = 0; j < len; j++) {
			if (!((name[j] >= 'a' && name[j] <= 'z') ||
			      (name[j] >= 'A' && name[j] <= 'Z') ||
			      (j && name[j] >= '0' && name[j] <= '9') ||
			      name[j] == '_')) {
				ret = -EINVAL;
				goto close;
			}
		}
	}
close:
	yz_policy_close(file);
out:
	if (saved)
		revert_creds(saved);

	return ret;
}

void yz_fixed_policy_latch(void)
{
	mutex_lock(&yz_fixed_lock);
	if (!yz_fixed_latched) {
		yz_fixed_status.error = yz_fixed_read_config();
		yz_fixed_status.requested = yz_fixed_config.enabled == 1;
		yz_fixed_apply_pending =
		    yz_fixed_status.requested && !yz_fixed_status.error;
		yz_fixed_latched = true;
	}
	mutex_unlock(&yz_fixed_lock);
}

int yz_fixed_policy_boot(bool explicit_enable)
{
	int ret = 0;

	yz_fixed_policy_latch();
	mutex_lock(&yz_fixed_lock);
	if (!yz_fixed_status.initialized) {
		int previous = yz_fixed_status.error;

		if (yz_fixed_apply_pending) {
			/* The early observer runs on exec. Retry allocation
			 * failures only when a caller actually requests feature
			 * activation. */
			if (!explicit_enable && previous == -ENOMEM) {
				ret = previous;
				goto out;
			}
			yz_fixed_set_missing(&yz_fixed_status, "", "", "", 0);
			yz_fixed_status.error = yz_fixed_policy_apply(
			    &yz_fixed_config, &yz_fixed_status);
			if (!yz_fixed_status.error)
				smp_store_release(&yz_fixed_status.active, 1);
			else if (yz_fixed_status.error == -EAGAIN ||
				 yz_fixed_status.error == -ENOMEM)
				ret = yz_fixed_status.error;
		}
		if (!ret)
			yz_fixed_status.initialized = 1;
		if (!ret || ret != previous)
			pr_info(
			    "yukizygisk: fixed policy requested=%u active=%u "
			    "error=%d\n",
			    yz_fixed_status.requested, yz_fixed_status.active,
			    yz_fixed_status.error);
	}
out:
	mutex_unlock(&yz_fixed_lock);
	return ret;
}

bool yz_fixed_policy_active(void)
{
	return smp_load_acquire(&yz_fixed_status.active) != 0;
}

void yz_fixed_set_missing(struct yz_fixed_policy_status *status,
			  const char *source, const char *target,
			  const char *class_name, u32 av)
{
	strscpy(status->source, source, sizeof(status->source));
	strscpy(status->target, target, sizeof(status->target));
	strscpy(status->class_name, class_name, sizeof(status->class_name));
	status->missing_av = av;
}

void yz_fixed_policy_missing(const char *source, const char *target,
			     const char *class_name, u32 av)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_fixed_missing_lock, flags);
	yz_fixed_set_missing(&yz_fixed_status, source, target, class_name, av);
	spin_unlock_irqrestore(&yz_fixed_missing_lock, flags);
}

void yz_fixed_policy_status(struct yz_fixed_policy_status *status)
{
	unsigned long flags;

	mutex_lock(&yz_fixed_lock);
	spin_lock_irqsave(&yz_fixed_missing_lock, flags);
	*status = yz_fixed_status;
	spin_unlock_irqrestore(&yz_fixed_missing_lock, flags);
	if (status->active) {
		struct yz_fixed_policy_status detail = {};

		status->error = yz_fixed_policy_live_error(&detail);
		if (status->error) {
			yz_fixed_set_missing(status, detail.source,
					     detail.target, detail.class_name,
					     detail.missing_av);
		}
	}
	mutex_unlock(&yz_fixed_lock);
}
