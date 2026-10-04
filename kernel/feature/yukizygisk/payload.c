#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/version.h>

#include "internal.h"
#include "ksu.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

void yz_close_current_fd(int fd)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	ksys_close(fd);
#else
	close_fd(fd);
#endif
}

void yz_cache_name(char *buf, size_t len)
{
	size_t i;

	if (!len)
		return;
	for (i = 0; i + 1 < len && i < sizeof(YZ_VMA_NAME) - 1; i++)
		buf[i] = YZ_VMA_NAME[i];
	buf[i] = '\0';
}

int yz_stage_fd(const char *path, const char *name,
		struct ksu_file_load_policy *policy_state)
{
	const struct cred *old_cred;
	struct file *src, *mfd;
	void *buf;
	loff_t sz, pos;
	ssize_t r;
	int fd, ret;

	/* Access the payload with KernelSU credentials. */
	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;

	src = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(src)) {
		if (old_cred)
			revert_creds(old_cred);
		pr_info("yukizygisk: payload open failed path=%s err=%ld\n",
			path, PTR_ERR(src));
		return -ENOENT;
	}
	if (!S_ISREG(file_inode(src)->i_mode)) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -EINVAL;
	}

	sz = i_size_read(file_inode(src));
	if (sz <= 0 || sz > YZ_LOADER_MAX_SZ) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -EINVAL;
	}

	buf = kvmalloc(sz, GFP_KERNEL);
	if (!buf) {
		filp_close(src, NULL);
		if (old_cred)
			revert_creds(old_cred);
		return -ENOMEM;
	}
	pos = 0;
	r = kernel_read(src, buf, sz, &pos);

	if (old_cred) {
		revert_creds(old_cred);
		old_cred = NULL;
	}

	if (r != sz) {
		pr_info("yukizygisk: payload read incomplete path=%s read=%zd "
			"size=%lld\n",
			path, r, (long long)sz);
		filp_close(src, NULL);
		kvfree(buf);
		return r < 0 ? (int)r : -EIO;
	}

	filp_close(src, NULL);

	mfd = shmem_file_setup(name, sz, 0);
	if (IS_ERR(mfd)) {
		long err = PTR_ERR(mfd);

		pr_info("yukizygisk: payload shmem creation failed name=%s "
			"err=%ld\n",
			name, err);
		kvfree(buf);
		return err;
	}
	/* shmem_file_setup lacks FMODE_PREAD/PWRITE by default. */
	mfd->f_mode |= FMODE_PREAD | FMODE_PWRITE | FMODE_LSEEK;
	pos = 0;
	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	r = kernel_write(mfd, buf, sz, &pos);
	if (old_cred) {
		revert_creds(old_cred);
		old_cred = NULL;
	}
	kvfree(buf);
	if (r != sz) {
		pr_info("yukizygisk: payload staging incomplete path=%s "
			"written=%zd size=%lld\n",
			path, r, (long long)sz);
		fput(mfd);
		return r < 0 ? (int)r : -EIO;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(mfd);
		return fd;
	}
	if (policy_state) {
		ret = ksu_file_load_policy_allow_current(mfd, policy_state);
		if (ret) {
			pr_info(
			    "yukizygisk: staged payload policy update failed "
			    "path=%s err=%d\n",
			    path, ret);
			put_unused_fd(fd);
			fput(mfd);
			return ret;
		}
	}
	fd_install(fd, mfd); /* consumes the shmem reference */

	pr_info("yukizygisk: payload staged path=%s size=%lld fd=%d\n", path,
		(long long)sz, fd);
	return fd;
}

int yz_stage_file_fd(const char *path,
		     struct ksu_file_load_policy *policy_state)
{
	const struct cred *old_cred;
	struct file *file;
	loff_t sz;
	int fd;
	int ret;

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;
	file = filp_open(path, O_RDONLY, 0);
	if (old_cred)
		revert_creds(old_cred);
	if (IS_ERR(file)) {
		pr_info(
		    "yukizygisk: file payload open failed path=%s err=%ld\n",
		    path, PTR_ERR(file));
		return PTR_ERR(file);
	}
	if (!S_ISREG(file_inode(file)->i_mode)) {
		filp_close(file, NULL);
		return -EINVAL;
	}

	sz = i_size_read(file_inode(file));
	if (sz <= 0 || sz > YZ_LOADER_MAX_SZ) {
		filp_close(file, NULL);
		return -EINVAL;
	}

	if (policy_state) {
		ret = ksu_file_load_policy_allow_current(file, policy_state);
		if (ret) {
			pr_info("yukizygisk: file payload policy update failed "
				"path=%s err=%d\n",
				path, ret);
			filp_close(file, NULL);
			return ret;
		}
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		filp_close(file, NULL);
		return fd;
	}
	fd_install(fd, file);

	pr_info("yukizygisk: file payload staged path=%s size=%lld fd=%d\n",
		path, (long long)sz, fd);
	return fd;
}
