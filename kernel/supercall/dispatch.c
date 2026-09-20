#include <linux/rculist.h>
#include "infra/mount_policy.h"
#include "kasumi_entrypoints.h"
#include <asm/unistd.h>
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/magic.h>
#include <linux/ptrace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seccomp.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "policy/feature.h"
#include "feature/selinux_hide.h"
#include "feature/sucompat.h"
#include "feature/sucompat_prompt.h"
#include "feature/sucompat_vfs.h"
#include "infra/file_wrapper.h"
#include "feature/kernel_umount.h"
#include "extension/uts_view.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "manager/apk_sign.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "manager/manager_identity.h"
#ifndef CONFIG_KSU_DISABLE_MANAGER
#include "manager/dynamic_manager.h"
#include "manager/throne_tracker.h"
#endif // #ifndef CONFIG_KSU_DISABLE_MANAGER
#include "infra/seccomp_cache.h"
#include "selinux/selinux.h"
#include "sulog/event.h"
#include "sulog/fd.h"
#include "supercall/supercall.h"
#include "supercall/internal.h"
#ifdef CONFIG_KSU_YUKIZYGISK
#include "feature/yukizygisk/api.h"
#include "uapi/yukizygisk.h"
#endif // #ifdef CONFIG_KSU_YUKIZYGISK
#include "hook/syscall_hook_manager.h"

#ifndef fd_file
#define fd_file(fd) ((fd).file)
#endif // #ifndef fd_file

#ifdef CONFIG_KSU_SUPERKEY
#include "manager/superkey.h"
#endif // #ifdef CONFIG_KSU_SUPERKEY

#define KSU_DYNAMIC_MANAGER_COMPAT_VERSION 35023
#define KSU_DYNAMIC_MANAGER_COMPAT_FULL_VERSION "v4.0.0-yukisucompat@YukiSU"

static int do_grant_root(void __user *arg)
{
	int ret;
	__u32 audit_uid = current_uid().val;
	__u32 audit_euid = current_euid().val;

	// we already check uid above on allowed_for_su()

	pr_info("allow root for: %d\n", audit_uid);
	ret = escape_with_root_profile();
	ksu_sulog_emit_grant_root(ret, audit_uid, audit_euid, GFP_KERNEL);

	return ret;
}

static int do_get_info(void __user *arg)
{
	struct ksu_get_info_cmd cmd = {.version = KERNEL_SU_VERSION,
				       .flags = 0};

	if (ksu_is_current_dynamic_manager())
		cmd.version = KSU_DYNAMIC_MANAGER_COMPAT_VERSION;

	cmd.flags |= KSU_GET_INFO_FLAG_LKM;
	if (ksu_bundled)
		cmd.flags |= KSU_GET_INFO_FLAG_BUNDLED;
	if (is_manager()) {
		cmd.flags |= KSU_GET_INFO_FLAG_MANAGER;
	}
	if (ksu_late_loaded) {
		cmd.flags |= KSU_GET_INFO_FLAG_LATE_LOAD;
	}
	cmd.features = KSU_FEATURE_MAX;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_load_mode(void __user *arg)
{
	struct ksu_get_load_mode_cmd cmd = {.mode = KSU_LOAD_MODE_RAMDISK,
					    .flags = 0};

	if (ksu_late_loaded)
		cmd.mode = KSU_LOAD_MODE_LATE;
	else if (ksu_imgpatch_loaded)
		cmd.mode = KSU_LOAD_MODE_IMAGE_PATCH;

	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		return -EFAULT;
	return 0;
}

static int do_report_event(void __user *arg)
{
	struct ksu_report_event_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	switch (cmd.event) {
	case EVENT_POST_FS_DATA: {
		static bool post_fs_data_lock = false;
		if (!post_fs_data_lock) {
			post_fs_data_lock = true;
			if (ksu_late_loaded) {
				pr_info("post-fs-data skipped (late load)\n");
			} else {
				pr_info("post-fs-data triggered\n");
				on_post_fs_data();
			}
		}
		break;
	}
	case EVENT_BOOT_COMPLETED: {
		static bool boot_complete_lock = false;
		if (!boot_complete_lock) {
			boot_complete_lock = true;
			if (ksu_late_loaded) {
				pr_info("boot_complete skipped (late load)\n");
				ksu_selinux_hide_drop_backup_if_unused();
			} else {
				pr_info("boot_complete triggered\n");
				on_boot_completed();
			}
		}
		break;
	}
	case EVENT_MODULE_MOUNTED: {
		pr_info("module mounted!\n");
		on_module_mounted();
		break;
	}
	default:
		break;
	}

	return 0;
}

static int do_set_sepolicy(void __user *arg)
{
	struct ksu_set_sepolicy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	return handle_sepolicy((void __user *)cmd.data, cmd.data_len);
}

static int do_check_safemode(void __user *arg)
{
	struct ksu_check_safemode_cmd cmd;

	cmd.in_safe_mode = ksu_is_safe_mode();

	if (cmd.in_safe_mode) {
		pr_warn("safemode enabled!\n");
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("check_safemode: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_allow_list_common(void __user *arg, bool allow)
{
	int *arr = NULL;
	int err = 0;
	u16 count;
	u32 out_count;
	static const u16 kSize = 128;

	arr = kmalloc(sizeof(int) * kSize, GFP_KERNEL);
	if (!arr)
		return -ENOMEM;

	if (!ksu_get_allow_list(arr, kSize, &count, NULL, allow)) {
		err = -EFAULT;
		goto out;
	}

	out_count = count;

	if (copy_to_user(arg + offsetof(struct ksu_get_allow_list_cmd, count),
			 &out_count, sizeof(u32))) {
		pr_err("get_allow_list: copy_to_user count failed\n");
		err = -EFAULT;
		goto out;
	}

	if (copy_to_user(arg, arr, sizeof(u32) * count)) {
		pr_err("get_allow_list: copy_to_user uids failed\n");
		err = -EFAULT;
	}

out:
	kfree(arr);
	return err;
}

static int do_get_allow_list(void __user *arg)
{
	return do_get_allow_list_common(arg, true);
}

static int do_get_deny_list(void __user *arg)
{
	return do_get_allow_list_common(arg, false);
}

static int do_new_get_allow_list_common(void __user *arg, bool allow)
{
	struct ksu_new_get_allow_list_cmd cmd;
	int *arr = NULL;
	int err = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	if (cmd.count) {
		arr = kmalloc(sizeof(int) * cmd.count, GFP_KERNEL);
		if (!arr)
			return -ENOMEM;
	}

	if (!ksu_get_allow_list(arr, cmd.count, &cmd.count, &cmd.total_count,
				allow)) {
		err = -EFAULT;
		goto out;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("new_get_allow_list: copy_to_user count failed\n");
		err = -EFAULT;
		goto out;
	}

	if (cmd.count && arr &&
	    copy_to_user(
		((struct ksu_new_get_allow_list_cmd __user *)arg)->uids, arr,
		sizeof(int) * cmd.count)) {
		pr_err("new_get_allow_list: copy_to_user uids failed\n");
		err = -EFAULT;
	}

out:
	kfree(arr);
	return err;
}

static int do_new_get_allow_list(void __user *arg)
{
	return do_new_get_allow_list_common(arg, true);
}

static int do_new_get_deny_list(void __user *arg)
{
	return do_new_get_allow_list_common(arg, false);
}

static int do_uid_granted_root(void __user *arg)
{
	struct ksu_uid_granted_root_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.granted = ksu_is_allow_uid_for_current(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_granted_root: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_uid_should_umount(void __user *arg)
{
	struct ksu_uid_should_umount_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		return -EFAULT;
	}

	cmd.should_umount = ksu_uid_should_umount(cmd.uid);

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("uid_should_umount: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_manager_appid(void __user *arg)
{
	struct ksu_get_manager_appid_cmd cmd;

	cmd.appid = ksu_get_manager_appid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_manager_appid: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_manager_uid(void __user *arg)
{
	struct ksu_get_manager_uid_cmd cmd;

	cmd.uid = ksu_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_manager_uid: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_get_app_profile(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_POLICY
	return -EOPNOTSUPP;
#else
	uid_t uid;
	struct app_profile *profile;
	int ret = 0;

	if (copy_from_user(
		&uid,
		(char __user *)arg +
		    offsetof(struct ksu_get_app_profile_cmd, profile.curr_uid),
		sizeof(uid))) {
		pr_err("get_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	profile = ksu_get_app_profile(uid);
	if (!profile) {
		ret = -ENOENT;
	} else {
		if (copy_to_user(
			(char __user *)arg +
			    offsetof(struct ksu_get_app_profile_cmd, profile),
			profile, sizeof(*profile))) {
			pr_err("get_app_profile: copy_to_user failed\n");
			ret = -EFAULT;
		}
		ksu_put_app_profile(profile);
	}

	return ret;
#endif // #ifdef CONFIG_KSU_DISABLE_POLICY
}

static int do_set_app_profile(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_POLICY
	return -EOPNOTSUPP;
#else
	struct ksu_set_app_profile_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_app_profile: copy_from_user failed\n");
		return -EFAULT;
	}

	if (!ksu_set_app_profile(&cmd.profile, true)) {
		return -EFAULT;
	}

	return 0;
#endif // #ifdef CONFIG_KSU_DISABLE_POLICY
}

/* Only persist the two policy shapes exposed by the su prompt. */
static int do_magisk_persist(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_POLICY
	return -EOPNOTSUPP;
#else
	struct ksu_magisk_persist_cmd cmd;
	struct app_profile profile;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	cmd.package[sizeof(cmd.package) - 1] = '\0';
	if (!cmd.package[0])
		return -EINVAL;

	memset(&profile, 0, sizeof(profile));
	profile.version = KSU_APP_PROFILE_VER;
	strscpy(profile.key, cmd.package, sizeof(profile.key));
	profile.curr_uid = cmd.uid;
	if (cmd.allow) {
		profile.allow_su = true;
		profile.rp_config.use_default = true;
	} else {
		profile.allow_su = false;
		profile.nrp_config.use_default = false;
		profile.nrp_config.profile.umount_modules = true;
	}

	if (!ksu_set_app_profile(&profile, true))
		return -EFAULT;
	return 0;
#endif // #ifdef CONFIG_KSU_DISABLE_POLICY
}

static int do_get_sulog_fd(void __user *arg)
{
	struct ksu_get_sulog_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_sulog_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	if (cmd.flags) {
		pr_err("get_sulog_fd: unsupported flags 0x%x\n", cmd.flags);
		return -EINVAL;
	}

	return ksu_install_sulog_fd();
}

static int do_get_su_prompt_fd(void __user *arg)
{
	struct ksu_get_su_prompt_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.flags)
		return -EINVAL;
	return ksu_sucompat_prompt_install_fd();
}

static int do_submit_su_prompt(void __user *arg)
{
	struct ksu_su_prompt_verdict verdict;

	if (copy_from_user(&verdict, arg, sizeof(verdict)))
		return -EFAULT;
	return ksu_sucompat_prompt_submit(&verdict);
}

static int do_su_prompt_ready(void __user *arg)
{
	struct ksu_su_prompt_key key;

	if (copy_from_user(&key, arg, sizeof(key)))
		return -EFAULT;
	return ksu_sucompat_prompt_ready(&key);
}

static int do_set_init_pgrp(void __user *arg)
{
	int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	struct pid *pids[PIDTYPE_MAX] = {0};
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	struct task_struct *p = current->group_leader;
	struct pid *init_group = task_pgrp(&init_task);

	write_lock_irq(&tasklist_lock);
	err = -EPERM;
	if (task_session(p) != task_session(&init_task))
		goto out;

	err = 0;
	if (task_pgrp(p) != init_group) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
		change_pid(pids, p, PIDTYPE_PGID, init_group);
#else
		change_pid(p, PIDTYPE_PGID, init_group);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	}

out:
	write_unlock_irq(&tasklist_lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	free_pids(pids);
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
	return err;
}

static int do_get_feature(void __user *arg)
{
	struct ksu_get_feature_cmd cmd;
	bool supported;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_get_feature(cmd.feature_id, &cmd.value, &supported);
	cmd.supported = supported ? 1 : 0;

	if (ret && supported) {
		pr_err("get_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_feature: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_set_feature(void __user *arg)
{
	struct ksu_set_feature_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("set_feature: copy_from_user failed\n");
		return -EFAULT;
	}

	ret = ksu_set_feature(cmd.feature_id, cmd.value);
	if (ret) {
		pr_err("set_feature: failed for feature %u: %d\n",
		       cmd.feature_id, ret);
		return ret;
	}

	return 0;
}

static int do_get_su_path(void __user *arg)
{
	struct ksu_su_path_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;
	ret = ksu_sucompat_vfs_get_config(config);
	if (!ret && copy_to_user(arg, config, sizeof(*config)))
		ret = -EFAULT;
	kfree(config);
	return ret;
}

static int do_set_su_path(void __user *arg)
{
	struct ksu_su_path_config *config;
	int ret;

	config = memdup_user(arg, sizeof(*config));
	if (IS_ERR(config))
		return PTR_ERR(config);
	ret = ksu_sucompat_vfs_set_config(config);
	kfree(config);
	return ret;
}

static int do_get_uts_view_config(void __user *arg)
{
	struct ksu_uts_view_config config = {};
	int ret;

	ret = ksu_uts_view_get_config(&config);
	if (ret)
		return ret;
	if (copy_to_user(arg, &config, sizeof(config)))
		return -EFAULT;
	return 0;
}

static int do_set_uts_view_config(void __user *arg)
{
	struct ksu_uts_view_config config = {};

	if (copy_from_user(&config, arg, sizeof(config)))
		return -EFAULT;
	return ksu_uts_view_set_config(&config);
}

static int do_get_uts_view_status(void __user *arg)
{
	struct ksu_uts_view_status status = {};
	int ret;

	ret = ksu_uts_view_get_status(&status);
	if (ret)
		return ret;
	if (copy_to_user(arg, &status, sizeof(status)))
		return -EFAULT;
	return 0;
}

static int do_get_wrapper_fd(void __user *arg)
{
	if (!ksu_file_sid) {
		return -EINVAL;
	}

	struct ksu_get_wrapper_fd_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("get_wrapper_fd: copy_from_user failed\n");
		return -EFAULT;
	}

	return ksu_install_file_wrapper(cmd.fd);
}

static int do_manage_mark(void __user *arg)
{
	struct ksu_manage_mark_cmd cmd;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("manage_mark: copy_from_user failed\n");
		return -EFAULT;
	}

	switch (cmd.operation) {
	case KSU_MARK_GET: {
		// Get task mark status
		ret = ksu_get_task_mark(cmd.pid);
		if (ret < 0) {
			pr_err("manage_mark: get failed for pid %d: %d\n",
			       cmd.pid, ret);
			return ret;
		}
		cmd.result = (u32)ret;
		break;
	}
	case KSU_MARK_MARK: {
		if (cmd.pid == 0) {
			ksu_mark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, true);
			if (ret < 0) {
				pr_err("manage_mark: set_mark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_UNMARK: {
		if (cmd.pid == 0) {
			ksu_unmark_all_process();
		} else {
			ret = ksu_set_task_mark(cmd.pid, false);
			if (ret < 0) {
				pr_err("manage_mark: set_unmark failed for pid "
				       "%d: %d\n",
				       cmd.pid, ret);
				return ret;
			}
		}
		break;
	}
	case KSU_MARK_REFRESH: {
		ksu_mark_running_process();
		pr_info("manage_mark: refreshed running processes\n");
		break;
	}
	default: {
		pr_err("manage_mark: invalid operation %u\n", cmd.operation);
		return -EINVAL;
	}
	}
	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("manage_mark: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

static int do_nuke_ext4_sysfs(void __user *arg)
{
	struct ksu_nuke_ext4_sysfs_cmd cmd;
	char mnt[256];
	long ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (!cmd.arg)
		return -EINVAL;

	memset(mnt, 0, sizeof(mnt));

	/*
	 * Hardening: validate user pointer before copying.
	 * This is adapted from upstream KernelSU's nuke_ext4_sysfs safety
	 * fixes to avoid panics on bad userspace pointers.
	 */
	if (!access_ok((const void __user *)(uintptr_t)cmd.arg, sizeof(mnt))) {
		pr_err("nuke ext4: invalid userspace pointer\n");
		return -EFAULT;
	}

	ret = strncpy_from_user(mnt, (const char __user *)(uintptr_t)cmd.arg,
				sizeof(mnt));
	if (ret < 0) {
		pr_err("nuke ext4 copy mnt failed: %ld\\n", ret);
		return -EFAULT;
	}

	if (ret == sizeof(mnt)) {
		pr_err("nuke ext4 mnt path too long\\n");
		return -ENAMETOOLONG;
	}

	pr_info("do_nuke_ext4_sysfs: %s\n", mnt);

	return nuke_ext4_sysfs(mnt);
}

struct list_head mount_list = LIST_HEAD_INIT(mount_list);
DECLARE_RWSEM(mount_list_lock);

static int add_try_umount(void __user *arg)
{
	struct mount_entry *new_entry, *entry, *tmp;
	struct ksu_add_try_umount_cmd cmd;
	char buf[256] = {0};

	if (copy_from_user(&cmd, arg, sizeof cmd))
		return -EFAULT;

	switch (cmd.mode) {
	case KSU_UMOUNT_WIPE:
		ksu_mount_policy_reset();
		return 0;

	case KSU_UMOUNT_ADD: {
		/* Hardening: validate userspace pointer before copy */
		if (!access_ok((const char __user *)cmd.arg, sizeof(buf)))
			return -EFAULT;

		long len =
		    strncpy_from_user(buf, (const char __user *)cmd.arg, 256);
		if (len <= 0)
			return -EFAULT;

		buf[sizeof(buf) - 1] = '\0';

		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry)
			return -ENOMEM;

		new_entry->umountable = kstrdup(buf, GFP_KERNEL);
		if (!new_entry->umountable) {
			kfree(new_entry);
			return -1;
		}

		ksu_capture_mount_identity(new_entry);
		down_write(&mount_list_lock);
		ksu_mount_policy_begin_update();

		// Refresh identity when the same mountpoint is registered
		// again.
		list_for_each_entry (entry, &mount_list, list) {
			if (!strcmp(entry->umountable, buf)) {
				if (new_entry->mount_root &&
				    new_entry->mount_fstype) {
					new_entry->flags = entry->flags;
					list_replace_rcu(&entry->list,
							 &new_entry->list);
					ksu_retire_mount_entry(entry);
					new_entry = NULL;
				}
				ksu_mount_policy_changed();
				up_write(&mount_list_lock);
				if (new_entry)
					ksu_free_mount_entry(new_entry);
				return 0;
			}
		}

		// now check flags and add
		// this also serves as a null check
		if (cmd.flags)
			new_entry->flags = cmd.flags;
		else
			new_entry->flags = 0;

		// debug
		list_add_rcu(&new_entry->list, &mount_list);
		ksu_mount_policy_changed();
		up_write(&mount_list_lock);
		pr_info("cmd_add_try_umount: %s added!\n", buf);

		return 0;
	}

	// this is just strcmp'd wipe anyway
	case KSU_UMOUNT_DEL: {
		/* Hardening: validate userspace pointer before copy */
		if (!access_ok((const char __user *)cmd.arg, 255))
			return -EFAULT;

		long len =
		    strncpy_from_user(buf, (const char __user *)cmd.arg, 255);
		if (len <= 0)
			return -EFAULT;

		buf[sizeof(buf) - 1] = '\0';

		down_write(&mount_list_lock);
		ksu_mount_policy_begin_update();
		list_for_each_entry_safe (entry, tmp, &mount_list, list) {
			if (!strcmp(entry->umountable, buf)) {
				pr_info(
				    "cmd_add_try_umount: entry removed: %s\n",
				    entry->umountable);
				list_del_rcu(&entry->list);
				ksu_retire_mount_entry(entry);
			}
		}
		ksu_mount_policy_changed();
		up_write(&mount_list_lock);

		return 0;
	}

	default: {
		pr_err("cmd_add_try_umount: invalid operation %u\n", cmd.mode);
		return -EINVAL;
	}

	} // switch(cmd.mode)

	return 0;
}

static int list_try_umount(void __user *arg)
{
	struct ksu_list_try_umount_cmd cmd;
	struct mount_entry *entry;
	char *output_buf;
	size_t output_size;
	size_t offset = 0;
	int ret = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	output_size = cmd.buf_size ? cmd.buf_size : 4096;

	if (!cmd.arg || output_size == 0)
		return -EINVAL;

	output_buf = kzalloc(output_size, GFP_KERNEL);
	if (!output_buf)
		return -ENOMEM;

	offset += snprintf(output_buf + offset, output_size - offset,
			   "Mount Point\tFlags\n");
	offset += snprintf(output_buf + offset, output_size - offset,
			   "----------\t-----\n");

	down_read(&mount_list_lock);
	list_for_each_entry (entry, &mount_list, list) {
		int written =
		    snprintf(output_buf + offset, output_size - offset,
			     "%s\t%u\n", entry->umountable, entry->flags);
		if (written < 0) {
			ret = -EFAULT;
			break;
		}
		if (written >= (int)(output_size - offset)) {
			ret = -ENOSPC;
			break;
		}
		offset += written;
	}
	up_read(&mount_list_lock);

	if (ret == 0) {
		if (copy_to_user((void __user *)cmd.arg, output_buf, offset))
			ret = -EFAULT;
	}

	kfree(output_buf);
	return ret;
}

static int do_set_dynamic_managers(void __user *arg)
{
#ifdef CONFIG_KSU_DISABLE_MANAGER
	return -EOPNOTSUPP;
#else
	struct ksu_dynamic_manager_cmd cmd;
	struct ksu_dynamic_manager_sign *signs = NULL;
	bool need_rescan = false;
	size_t bytes;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (cmd.count > KSU_DYNAMIC_MANAGER_MAX_SIGNS)
		return -EINVAL;

	if (cmd.count) {
		if (!cmd.signs)
			return -EINVAL;

		bytes = sizeof(*signs) * cmd.count;
		signs = kmalloc(bytes, GFP_KERNEL);
		if (!signs)
			return -ENOMEM;

		if (copy_from_user(signs,
				   (const void __user *)(uintptr_t)cmd.signs,
				   bytes)) {
			kfree(signs);
			return -EFAULT;
		}
	}

	ret = ksu_dynamic_manager_set(signs, cmd.count, &need_rescan);
	kfree(signs);

	if (!ret && need_rescan)
		ksu_request_manager_rescan();

	return ret;
#endif // #ifdef CONFIG_KSU_DISABLE_MANAGER
}

static int do_get_dynamic_managers(void __user *arg)
{
	struct ksu_get_dynamic_managers_cmd cmd;
	struct ksu_dynamic_manager_app *apps = NULL;
	u32 count = 0;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;

	if (cmd.count > KSU_DYNAMIC_MANAGER_MAX_APPS)
		return -EINVAL;

	if (cmd.count) {
		if (!cmd.apps)
			return -EINVAL;

		apps = kcalloc(cmd.count, sizeof(*apps), GFP_KERNEL);
		if (!apps)
			return -ENOMEM;
	}

	cmd.total_count = 0;
#ifndef CONFIG_KSU_DISABLE_MANAGER
	count = ksu_dynamic_manager_get_apps(apps, cmd.count, &cmd.total_count);
#endif // #ifndef CONFIG_KSU_DISABLE_MANAGER
	cmd.count = count;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		kfree(apps);
		return -EFAULT;
	}

	if (cmd.count && apps &&
	    copy_to_user((void __user *)(uintptr_t)cmd.apps, apps,
			 sizeof(*apps) * cmd.count)) {
		kfree(apps);
		return -EFAULT;
	}

	kfree(apps);
	return 0;
}

// 100. GET_FULL_VERSION - Get full version string
static int do_get_full_version(void __user *arg)
{
	struct ksu_get_full_version_cmd cmd = {0};
	const char *version_full = KSU_VERSION_FULL;

	if (ksu_is_current_dynamic_manager())
		version_full = KSU_DYNAMIC_MANAGER_COMPAT_FULL_VERSION;

	strscpy(cmd.version_full, version_full, sizeof(cmd.version_full));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_full_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 101. HOOK_TYPE - Get hook type
static int do_get_hook_type(void __user *arg)
{
	struct ksu_hook_type_cmd cmd = {0};
	const char *type = "TSR Hook";

	strscpy(cmd.hook_type, type, sizeof(cmd.hook_type));

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("get_hook_type: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

// 107. SUPERKEY_AUTH - Authenticate with superkey (APatch-style)
#ifdef CONFIG_KSU_SUPERKEY

static int do_superkey_auth(void __user *arg)
{
	struct ksu_superkey_auth_cmd cmd;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd))) {
		pr_err("superkey_auth: copy_from_user failed\n");
		return -EFAULT;
	}

	cmd.superkey[sizeof(cmd.superkey) - 1] = '\0';

	ret = superkey_authenticate(cmd.superkey);
	cmd.result = ret;

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_auth: copy_to_user failed\n");
		return -EFAULT;
	}

	return ret;
}

// 108. SUPERKEY_STATUS - Get superkey status
static int do_superkey_status(void __user *arg)
{
	struct ksu_superkey_status_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.enabled = superkey_is_set();
	cmd.authenticated = superkey_is_manager();
	/*
	 * signature_ok: whether manager APK's signature verification
	 * has passed. In SuperKey-only mode (signature bypass), this
	 * will remain 0.
	 */
	cmd.signature_ok = is_manager_apk(NULL) ? 1 : 0;
	cmd.manager_uid = superkey_get_manager_uid();

	if (copy_to_user(arg, &cmd, sizeof(cmd))) {
		pr_err("superkey_status: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}
#endif // #ifdef CONFIG_KSU_SUPERKEY

// Lock the calling (already-root) thread and its children out of any further
// KernelSU escalation. Backs `su --ksu-no-new-privs`.
static int do_disable_escape_to_root(void __user *arg)
{
	set_thread_flag(TIF_KSU_DISABLE_ESCAPE_WITH_ROOT);
	return 0;
}

// Report the UAPI contract version via its own ioctl, keeping GET_INFO's number
// and struct stable across kernel/userspace version skew.
static int do_get_uapi_version(void __user *arg)
{
	__u32 v = KERNEL_SU_UAPI_VERSION;

	if (copy_to_user(arg, &v, sizeof(v))) {
		pr_err("get_uapi_version: copy_to_user failed\n");
		return -EFAULT;
	}

	return 0;
}

#ifdef CONFIG_KSU_YUKIZYGISK
static int do_yz_handoff(void __user *arg)
{
	return ksu_yukizygisk_handoff_module_fds(arg);
}

static int do_yz_set_dlopen(void __user *arg)
{
	struct yz_dlopen_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	ksu_yukizygisk_set_linker_offsets(cmd.dlopen_offset, cmd.dlsym_offset);
	return 0;
}

static int do_yz_set_dlopen32(void __user *arg)
{
	struct yz_dlopen_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	ksu_yukizygisk_set_compat_linker_offsets(cmd.dlopen_offset,
						 cmd.dlsym_offset);
	return 0;
}

static int do_yz_reload(void __user *arg)
{
	(void)arg;
	ksu_yukizygisk_emit_reload();
	return 0;
}

static int do_yz_set_yukilinker(void __user *arg)
{
	struct yz_yukilinker_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	ksu_yukizygisk_set_first_stage_loader(cmd.enabled != 0);
	return 0;
}

static int do_yz_set_native_targets(void __user *arg)
{
	struct yz_native_targets_cmd *cmd;
	int ret;

	cmd = memdup_user(arg, sizeof(*cmd));
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);
	ret = ksu_yukizygisk_set_native_targets(cmd);
	kfree(cmd);
	return ret;
}

static int do_yz_restore_native_load_policy(void __user *arg)
{
	struct yz_native_load_policy_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	return ksu_yukizygisk_restore_native_load_policy((pid_t)cmd.pid);
}

static int do_yz_get_safemode(void __user *arg)
{
	struct yz_safemode_status_cmd cmd;
	int ret;

	ret = ksu_yukizygisk_get_safemode(&cmd);
	if (ret)
		return ret;
	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		return -EFAULT;
	return 0;
}

static int do_yz_get_runtime(void __user *arg)
{
	struct yz_runtime_query_cmd cmd;
	struct yz_runtime_record *entries = NULL;
	void __user *user_entries;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.capacity > YZ_RUNTIME_RECORD_MAX ||
	    (cmd.capacity && !cmd.entries))
		return -EINVAL;

	user_entries = (void __user *)(uintptr_t)cmd.entries;
	if (cmd.capacity) {
		entries = kvcalloc(cmd.capacity, sizeof(*entries), GFP_KERNEL);
		if (!entries)
			return -ENOMEM;
	}
	ret = ksu_yukizygisk_get_runtime(entries, cmd.capacity, &cmd);
	if (ret)
		goto out;
	if (cmd.count &&
	    copy_to_user(user_entries, entries, sizeof(*entries) * cmd.count)) {
		ret = -EFAULT;
		goto out;
	}
	if (copy_to_user(arg, &cmd, sizeof(cmd)))
		ret = -EFAULT;
out:
	kvfree(entries);
	return ret;
}

static int do_yz_report_runtime(void __user *arg)
{
	struct yz_runtime_report_cmd cmd;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	cmd.module_id[sizeof(cmd.module_id) - 1] = '\0';
	return ksu_yukizygisk_report_runtime(&cmd);
}

static int do_yz_allow_module_load_policy(void __user *arg)
{
	struct yz_module_load_policy_cmd cmd;
	struct task_struct *task;
	const struct cred *cred;
	struct fd payload;
	struct file *file;
	bool directory;
	bool source_image;
	int ret;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (!cmd.pid || cmd.dirfd < 0)
		return -EINVAL;
	rcu_read_lock();
	task = get_pid_task(find_vpid(cmd.pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;
	cred = get_task_cred(task);
	payload = fdget(cmd.dirfd);
	file = fd_file(payload);
	if (!file) {
		ret = -EBADF;
		goto out_cred;
	}
	directory = S_ISDIR(file_inode(file)->i_mode);
	source_image = S_ISREG(file_inode(file)->i_mode) &&
		       file_inode(file)->i_sb->s_magic == TMPFS_MAGIC &&
		       (file->f_mode & FMODE_READ) &&
		       !(file->f_mode & FMODE_WRITE);
	if (!directory && !source_image) {
		ret = -EINVAL;
		goto out_fd;
	}
	if (!is_zygote(cred) &&
	    !(source_image &&
	      ksu_yukizygisk_is_native_runtime(
		  task->tgid, READ_ONCE(task->start_boottime)))) {
		ret = -EPERM;
		goto out_fd;
	}
	ret = ksu_yukizygisk_allow_module_load_policy(task->tgid, file, cred);
out_fd:
	fdput(payload);
out_cred:
	put_cred(cred);
	put_task_struct(task);
	pr_info("yukizygisk: module load policy request pid=%u fd=%d err=%d\n",
		cmd.pid, cmd.dirfd, ret);
	return ret;
}

struct yz_unmap_tw {
	struct callback_head cb;
	unsigned long addr[YZ_MAX_UNMAP_SEGS];
	unsigned long size[YZ_MAX_UNMAP_SEGS];
	unsigned int n;
	unsigned int retry;
};

/* Retry until the program counter leaves all requested ranges. */
static void yz_unmap_tw_func(struct callback_head *cb)
{
	struct yz_unmap_tw *tw = container_of(cb, struct yz_unmap_tw, cb);
	struct pt_regs *regs = task_pt_regs(current);
	unsigned long pc = regs ? instruction_pointer(regs) : 0;
	unsigned int i;

	for (i = 0; i < tw->n; i++) {
		if (pc >= tw->addr[i] && pc < tw->addr[i] + tw->size[i]) {
			if (++tw->retry < 16) {
				init_task_work(&tw->cb, yz_unmap_tw_func);
				if (!task_work_add(current, &tw->cb,
						   TWA_RESUME))
					return; /* re-queued; keep tw */
			}
			pr_warn("yukizygisk: core unmap skipped pid=%d "
				"pc=0x%lx retries=%u\n",
				current->pid, pc, tw->retry);
			kfree(tw);
			return;
		}
	}

	for (i = 0; i < tw->n; i++) {
		pr_info("yukizygisk: core segment unmap pid=%d address=0x%lx "
			"size=0x%lx\n",
			current->pid, tw->addr[i], tw->size[i]);
		vm_munmap(tw->addr[i], tw->size[i]);
	}
	kfree(tw);
}

static int do_yz_unmap_pid(void __user *arg)
{
	struct yz_unmap_pid_cmd cmd;
	struct task_struct *task;
	struct yz_unmap_tw *tw;
	unsigned int i;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.n_segs == 0 || cmd.n_segs > YZ_MAX_UNMAP_SEGS)
		return -EINVAL;

	rcu_read_lock();
	task = get_pid_task(find_vpid(cmd.pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	if (!is_appuid(task_uid(task).val)) {
		pr_info("yukizygisk: core unmap rejected pid=%u uid=%u\n",
			cmd.pid, task_uid(task).val);
		put_task_struct(task);
		return -EPERM;
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw) {
		put_task_struct(task);
		return -ENOMEM;
	}
	init_task_work(&tw->cb, yz_unmap_tw_func);
	tw->n = cmd.n_segs;
	for (i = 0; i < cmd.n_segs; i++) {
		tw->addr[i] = (unsigned long)cmd.addr[i];
		tw->size[i] = (unsigned long)cmd.size[i];
	}
	if (task_work_add(task, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		put_task_struct(task);
		return -ESRCH;
	}
	put_task_struct(task);
	pr_info("yukizygisk: core unmap scheduled pid=%u segments=%u\n",
		cmd.pid, cmd.n_segs);
	return 0;
}

static int do_yz_unmap_self(void __user *arg)
{
	struct yz_unmap_self_cmd cmd;
	struct yz_unmap_tw *tw;
	unsigned int i;

	if (!current->mm)
		return -EINVAL;
	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.n_segs == 0 || cmd.n_segs > YZ_MAX_UNMAP_SEGS)
		return -EINVAL;
	for (i = 0; i < cmd.n_segs; i++) {
		unsigned long a = (unsigned long)cmd.addr[i];
		unsigned long s = (unsigned long)cmd.size[i];

		if (a == 0 || s == 0 || a >= TASK_SIZE || s > TASK_SIZE ||
		    a + s < a || a + s > TASK_SIZE) {
			pr_warn("yukizygisk: invalid core segment pid=%d "
				"address=0x%lx size=0x%lx\n",
				current->pid, a, s);
			return -EINVAL;
		}
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw)
		return -ENOMEM;
	init_task_work(&tw->cb, yz_unmap_tw_func);
	tw->n = cmd.n_segs;
	for (i = 0; i < cmd.n_segs; i++) {
		tw->addr[i] = (unsigned long)cmd.addr[i];
		tw->size[i] = (unsigned long)cmd.size[i];
	}
	if (task_work_add(current, &tw->cb, TWA_RESUME)) {
		kfree(tw);
		return -ESRCH;
	}
	pr_info("yukizygisk: self core unmap scheduled pid=%d segments=%u\n",
		current->pid, cmd.n_segs);
	return 0;
}

/* COW-patch target text via access_process_vm(FOLL_FORCE|FOLL_WRITE). */
static int do_yz_patch_text(void __user *arg)
{
	struct yz_patch_text_cmd cmd;
	struct task_struct *task;
	int n;

	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (cmd.len == 0 || cmd.len > YZ_PATCH_TEXT_MAX)
		return -EINVAL;
	if (cmd.addr == 0 || cmd.addr >= TASK_SIZE ||
	    cmd.addr + cmd.len < cmd.addr || cmd.addr + cmd.len > TASK_SIZE)
		return -EINVAL;

	rcu_read_lock();
	task = get_pid_task(find_vpid(cmd.pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	/* zygiskd validates cmd.pid against the SO_PEERCRED peer PID. */

	n = access_process_vm(task, (unsigned long)cmd.addr, cmd.bytes, cmd.len,
			      FOLL_FORCE | FOLL_WRITE);
	put_task_struct(task);
	if (n != (int)cmd.len) {
		pr_warn("yukizygisk: text patch incomplete pid=%u "
			"address=0x%llx written=%d requested=%u\n",
			cmd.pid, cmd.addr, n, cmd.len);
		return -EFAULT;
	}
	pr_info("yukizygisk: text patched pid=%u address=0x%llx bytes=%u\n",
		cmd.pid, cmd.addr, cmd.len);
	return 0;
}
#endif // #ifdef CONFIG_KSU_YUKIZYGISK

// IOCTL handlers mapping table
static const struct ksu_ioctl_cmd_map ksu_ioctl_handlers[] = {
    {.cmd = KSU_IOCTL_GRANT_ROOT,
     .name = "GRANT_ROOT",
     .handler = do_grant_root,
     .perm_check = allowed_for_su},
    {.cmd = KSU_IOCTL_GET_INFO,
     .name = "GET_INFO",
     .handler = do_get_info,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_LOAD_MODE,
     .name = "GET_LOAD_MODE",
     .handler = do_get_load_mode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_REPORT_EVENT,
     .name = "REPORT_EVENT",
     .handler = do_report_event,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_SET_SEPOLICY,
     .name = "SET_SEPOLICY",
     .handler = do_set_sepolicy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_CHECK_SAFEMODE,
     .name = "CHECK_SAFEMODE",
     .handler = do_check_safemode,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_GET_ALLOW_LIST,
     .name = "GET_ALLOW_LIST",
     .handler = do_get_allow_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_DENY_LIST,
     .name = "GET_DENY_LIST",
     .handler = do_get_deny_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NEW_GET_ALLOW_LIST,
     .name = "NEW_GET_ALLOW_LIST",
     .handler = do_new_get_allow_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NEW_GET_DENY_LIST,
     .name = "NEW_GET_DENY_LIST",
     .handler = do_new_get_deny_list,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_GRANTED_ROOT,
     .name = "UID_GRANTED_ROOT",
     .handler = do_uid_granted_root,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_UID_SHOULD_UMOUNT,
     .name = "UID_SHOULD_UMOUNT",
     .handler = do_uid_should_umount,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_MANAGER_APPID,
     .name = "GET_MANAGER_APPID",
     .handler = do_get_manager_appid,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_MANAGER_UID,
     .name = "GET_MANAGER_UID",
     .handler = do_get_manager_uid,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_APP_PROFILE,
     .name = "GET_APP_PROFILE",
     .handler = do_get_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_SET_APP_PROFILE,
     .name = "SET_APP_PROFILE",
     .handler = do_set_app_profile,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_GET_FEATURE,
     .name = "GET_FEATURE",
     .handler = do_get_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_FEATURE,
     .name = "SET_FEATURE",
     .handler = do_set_feature,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_WRAPPER_FD,
     .name = "GET_WRAPPER_FD",
     .handler = do_get_wrapper_fd,
     .perm_check = manager_or_root,
     .allow_su_session = true},
    {.cmd = KSU_IOCTL_MANAGE_MARK,
     .name = "MANAGE_MARK",
     .handler = do_manage_mark,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_NUKE_EXT4_SYSFS,
     .name = "NUKE_EXT4_SYSFS",
     .handler = do_nuke_ext4_sysfs,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_ADD_TRY_UMOUNT,
     .name = "ADD_TRY_UMOUNT",
     .handler = add_try_umount,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_INIT_PGRP,
     .name = "SET_INIT_PGRP",
     .handler = do_set_init_pgrp,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GET_SULOG_FD,
     .name = "GET_SULOG_FD",
     .handler = do_get_sulog_fd,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GET_SU_PROMPT_FD,
     .name = "GET_SU_PROMPT_FD",
     .handler = do_get_su_prompt_fd,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_SUBMIT_SU_PROMPT,
     .name = "SUBMIT_SU_PROMPT",
     .handler = do_submit_su_prompt,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_SU_PROMPT_READY,
     .name = "SU_PROMPT_READY",
     .handler = do_su_prompt_ready,
     .perm_check = only_manager},
    {.cmd = KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT,
     .name = "DISABLE_ESCAPE_TO_ROOT",
     .handler = do_disable_escape_to_root,
     .perm_check = only_root,
     .allow_su_session = true},
    {.cmd = KSU_IOCTL_GET_UAPI_VERSION,
     .name = "GET_UAPI_VERSION",
     .handler = do_get_uapi_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_SET_DYNAMIC_MANAGERS,
     .name = "SET_DYNAMIC_MANAGERS",
     .handler = do_set_dynamic_managers,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_DYNAMIC_MANAGERS,
     .name = "GET_DYNAMIC_MANAGERS",
     .handler = do_get_dynamic_managers,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_MAGISK_PERSIST,
     .name = "MAGISK_PERSIST",
     .handler = do_magisk_persist,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_GET_SU_PATH,
     .name = "GET_SU_PATH",
     .handler = do_get_su_path,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_SU_PATH,
     .name = "SET_SU_PATH",
     .handler = do_set_su_path,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_UTS_VIEW_CONFIG,
     .name = "GET_UTS_VIEW_CONFIG",
     .handler = do_get_uts_view_config,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_SET_UTS_VIEW_CONFIG,
     .name = "SET_UTS_VIEW_CONFIG",
     .handler = do_set_uts_view_config,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_UTS_VIEW_STATUS,
     .name = "GET_UTS_VIEW_STATUS",
     .handler = do_get_uts_view_status,
     .perm_check = manager_or_root},
    {.cmd = KSU_IOCTL_GET_FULL_VERSION,
     .name = "GET_FULL_VERSION",
     .handler = do_get_full_version,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_HOOK_TYPE,
     .name = "GET_HOOK_TYPE",
     .handler = do_get_hook_type,
     .perm_check = manager_or_root},
#ifdef CONFIG_KSU_SUPERKEY
    {.cmd = KSU_IOCTL_SUPERKEY_AUTH,
     .name = "SUPERKEY_AUTH",
     .handler = do_superkey_auth,
     .perm_check = always_allow},
    {.cmd = KSU_IOCTL_SUPERKEY_STATUS,
     .name = "SUPERKEY_STATUS",
     .handler = do_superkey_status,
     .perm_check = always_allow},
#endif // #ifdef CONFIG_KSU_SUPERKEY
    {.cmd = KSU_IOCTL_LIST_TRY_UMOUNT,
     .name = "LIST_TRY_UMOUNT",
     .handler = list_try_umount,
     .perm_check = manager_or_root},
#ifdef CONFIG_KSU_YUKIZYGISK
    {.cmd = KSU_IOCTL_YZ_HANDOFF,
     .name = "YZ_HANDOFF",
     .handler = do_yz_handoff,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_SET_DLOPEN,
     .name = "YZ_SET_DLOPEN",
     .handler = do_yz_set_dlopen,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_SET_DLOPEN32,
     .name = "YZ_SET_DLOPEN32",
     .handler = do_yz_set_dlopen32,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_SET_YUKILINKER,
     .name = "YZ_SET_YUKILINKER",
     .handler = do_yz_set_yukilinker,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_SET_NATIVE_TARGETS,
     .name = "YZ_SET_NATIVE_TARGETS",
     .handler = do_yz_set_native_targets,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_RESTORE_NATIVE_LOAD_POLICY,
     .name = "YZ_RESTORE_NATIVE_LOAD_POLICY",
     .handler = do_yz_restore_native_load_policy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_GET_SAFEMODE,
     .name = "YZ_GET_SAFEMODE",
     .handler = do_yz_get_safemode,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_GET_RUNTIME,
     .name = "YZ_GET_RUNTIME",
     .handler = do_yz_get_runtime,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_REPORT_RUNTIME,
     .name = "YZ_REPORT_RUNTIME",
     .handler = do_yz_report_runtime,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_ALLOW_MODULE_LOAD_POLICY,
     .name = "YZ_ALLOW_MODULE_LOAD_POLICY",
     .handler = do_yz_allow_module_load_policy,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_UNMAP_PID,
     .name = "YZ_UNMAP_PID",
     .handler = do_yz_unmap_pid,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_UNMAP_SELF,
     .name = "YZ_UNMAP_SELF",
     .handler = do_yz_unmap_self,
     .perm_check = injected_app},
    {.cmd = KSU_IOCTL_YZ_PATCH_TEXT,
     .name = "YZ_PATCH_TEXT",
     .handler = do_yz_patch_text,
     .perm_check = only_root},
    {.cmd = KSU_IOCTL_YZ_RELOAD,
     .name = "YZ_RELOAD",
     .handler = do_yz_reload,
     .perm_check = manager_or_root},
#endif // #ifdef CONFIG_KSU_YUKIZYGISK
    {.cmd = 0, .name = NULL, .handler = NULL, .perm_check = NULL} // Sentinel
};

void ksu_supercall_dump_commands(void)
{
	int i;

	pr_info("KernelSU IOCTL Commands:\n");
	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		pr_info("  %-18s = 0x%08x\n", ksu_ioctl_handlers[i].name,
			ksu_ioctl_handlers[i].cmd);
	}
}

long ksu_supercall_handle_ioctl(const struct file *filp, unsigned int cmd,
				void __user *argp)
{
	int i;

#ifdef CONFIG_KSU_DEBUG
	pr_info("ksu ioctl: cmd=0x%x from uid=%d\n", cmd, current_uid().val);
#endif // #ifdef CONFIG_KSU_DEBUG

	if (_IOC_TYPE(cmd) == KSM_IOC_MAGIC) {
		if (!manager_or_root())
			return -EPERM;
#ifdef CONFIG_COMPAT
		if (in_compat_syscall())
			return -EOPNOTSUPP;
#endif
		return kasumi_handle_ioctl(cmd, argp);
	}

	for (i = 0; ksu_ioctl_handlers[i].handler; i++) {
		if (cmd == ksu_ioctl_handlers[i].cmd) {
			if (ksu_ioctl_handlers[i].perm_check &&
			    !ksu_ioctl_handlers[i].perm_check() &&
			    !(ksu_ioctl_handlers[i].allow_su_session &&
			      ksu_is_su_session_fd(filp))) {
				pr_warn("ksu ioctl: permission denied for "
					"cmd=0x%x uid=%d\n",
					cmd, current_uid().val);
				return -EPERM;
			}

			return ksu_ioctl_handlers[i].handler(argp);
		}
	}

	pr_warn("ksu ioctl: unsupported command 0x%x\n", cmd);
	return -ENOTTY;
}
