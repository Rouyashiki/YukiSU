#include "feature/sucompat_module_guard.h"
#include "feature/sucompat.h"
#include "policy/feature.h"
#include <linux/rwsem.h>
#include "kasumi_proc_read_hooks.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "kasumi_bootstrap.h"
#include "kasumi_config_guard.h"
#include "kasumi_runtime.h"
#include "kasumi_path_policy.h"
#include "kasumi_store.h"
#include "kasumi_fop_bridge.h"
#include "kasumi_entrypoints.h"
#include "kasumi_vfs_hooks.h"
#include "kasumi_vfs_view.h"
#include "kasumi_exec_view.h"
#include "kasumi_notify_view.h"
#include "kasumi_io_notify.h"
#include "kasumi_file_lock.h"
#include "kasumi_vnode.h"
#include "kasumi_rename_view.h"
#include "kasumi_live_rules.h"
#include "kasumi_iop_override.h"
#include "kasumi_dirhijack.h"
#include "kasumi_hide_rules.h"
#include "kasumi_sop_shadow.h"
#include "kasumi_fop_override.h"
#include "kasumi_fake_mountinfo.h"
#include "kasumi_fake_selinuxfs_access.h"

/* VFS callbacks outlive individual control requests; pin their host module. */
static bool kasumi_unload_pin_held;

static void kasumi_bootstrap_release_unload_pin(void)
{
	if (WARN_ON_ONCE(!READ_ONCE(kasumi_unload_pin_held)))
		return;
	WRITE_ONCE(kasumi_unload_pin_held, false);
	module_put(THIS_MODULE);
}

static noinline KASUMI_NOCFI void kasumi_resolve_system_dev(void)
{
	struct path sys_path = {};
	struct dentry *dentry;
	struct vfsmount *mnt;
	struct super_block *sb;
	int ret;

	ret = kern_path("/system", LOOKUP_FOLLOW, &sys_path);
	if (ret) {
		pr_warn(
		    "kasumi: could not resolve /system for stat spoofing: %d\n",
		    ret);
		return;
	}

	dentry = READ_ONCE(sys_path.dentry);
	mnt = READ_ONCE(sys_path.mnt);
	sb = dentry ? READ_ONCE(dentry->d_sb) : NULL;
	if (!dentry || !mnt || !sb) {
		pr_warn("kasumi: /system resolved to incomplete path (mnt=%p "
			"dentry=%p sb=%p), stat spoofing dev disabled\n",
			mnt, dentry, sb);
		if (dentry && mnt)
			path_put(&sys_path);
		return;
	}

	kasumi_system_dev = sb->s_dev;
	pr_info("kasumi: /system dev=%u:%u\n", MAJOR(kasumi_system_dev),
		MINOR(kasumi_system_dev));
	path_put(&sys_path);
}

static void kasumi_resolve_runtime_symbols(void)
{
	kasumi_notify_change = (void *)ksu_lookup_symbol("notify_change");
	kasumi_vfs_getxattr_addr = (void *)ksu_lookup_symbol("vfs_getxattr");
	kasumi_vfs_listxattr_addr = (void *)ksu_lookup_symbol("vfs_listxattr");
	kasumi_vfs_setxattr_addr = (void *)ksu_lookup_symbol("vfs_setxattr");
	kasumi_vfs_removexattr_addr =
	    (void *)ksu_lookup_symbol("vfs_removexattr");
	kasumi_mnt_want_write_addr =
	    (void *)ksu_lookup_symbol("mnt_want_write");
	kasumi_mnt_drop_write_addr =
	    (void *)ksu_lookup_symbol("mnt_drop_write");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	kasumi_lookup_one = (void *)ksu_lookup_symbol("lookup_one");
#else
	kasumi_lookup_one = (void *)ksu_lookup_symbol("lookup_one_len");
#endif
	kasumi_vfs_create = (void *)ksu_lookup_symbol("vfs_create");
	kasumi_vfs_mkdir = (void *)ksu_lookup_symbol("vfs_mkdir");
	kasumi_vfs_mknod = (void *)ksu_lookup_symbol("vfs_mknod");
	kasumi_vfs_symlink = (void *)ksu_lookup_symbol("vfs_symlink");
	kasumi_vfs_unlink = (void *)ksu_lookup_symbol("vfs_unlink");
	kasumi_vfs_rmdir = (void *)ksu_lookup_symbol("vfs_rmdir");
	kasumi_vfs_link = (void *)ksu_lookup_symbol("vfs_link");
	kasumi_vfs_rename = (void *)ksu_lookup_symbol("vfs_rename");
	kasumi_vfs_read = (void *)ksu_lookup_symbol("vfs_read");
	kasumi_rw_verify_area = (void *)kasumi_lookup_callable_quiet("rw_verify_area");
	kasumi_security_file_permission =
		(void *)kasumi_lookup_callable_quiet("security_file_permission");
	kasumi_fput_sync = (void *)kasumi_lookup_callable_quiet("__fput_sync");
	kasumi_vfs_write = (void *)ksu_lookup_symbol("vfs_write");
	kasumi_get_vfs_caps_from_disk =
	    (void *)ksu_lookup_symbol("get_vfs_caps_from_disk");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	kasumi_security_inode_getlsmprop =
		(void *)ksu_lookup_symbol("security_inode_getlsmprop");
#else
	kasumi_security_inode_getsecid =
		(void *)ksu_lookup_symbol("security_inode_getsecid");
#endif
	kasumi_security_inode_getsecctx =
	    (void *)ksu_lookup_symbol("security_inode_getsecctx");
	kasumi_security_inode_notifysecctx =
	    (void *)ksu_lookup_symbol("security_inode_notifysecctx");
	kasumi_security_release_secctx =
	    (void *)ksu_lookup_symbol("security_release_secctx");
	kasumi_d_absolute_path = (void *)ksu_lookup_symbol("d_absolute_path");
	kasumi_d_hash_and_lookup =
	    (void *)ksu_lookup_symbol("d_hash_and_lookup");
	kasumi_free_inode_nonrcu_ptr =
	    (void *)ksu_lookup_symbol("free_inode_nonrcu");
}

static int kasumi_start_views(void)
{
	int ret;

	pr_info("kasumi: initializing embedded VFS views\n");

	kasumi_filldir_cache = kmem_cache_create(
	    "kasumi_filldir", sizeof(struct kasumi_filldir_wrapper), 0,
	    SLAB_HWCACHE_ALIGN, NULL);
	if (!kasumi_filldir_cache) {
		pr_alert("kasumi: failed to create filldir slab cache\n");
		return -ENOMEM;
	}

	kasumi_resolve_runtime_symbols();

	hash_init(kasumi_paths);
	hash_init(kasumi_hide_paths);
	hash_init(kasumi_inject_dirs);
	hash_init(kasumi_xattr_sbs);
	hash_init(kasumi_merge_dirs);


	kasumi_resolve_system_dev();

	ret = kasumi_fake_mi_init();
	if (ret)
		pr_warn("kasumi: fake mountinfo unavailable: %d\n", ret);

	kasumi_proc_read_hooks_init();

	(void)kasumi_iop_override_init();
	ret = kasumi_fop_bridge_init();
	if (ret) {
		pr_err("kasumi: FATAL - old-KMI fops bridge unavailable: %d\n",
		       ret);
		goto err_fop_bridge;
	}
	ret = kasumi_vnode_handle_init();
	if (ret) {
		pr_err("kasumi: vnode file handle guard unavailable: %d\n", ret);
		goto err_fop_bridge;
	}
	ret = kasumi_io_notify_init();
	if (ret) {
		pr_err("kasumi: I/O notification relay unavailable: %d\n", ret);
		goto err_fop_bridge;
	}
	ret = kasumi_vnode_aio_init();
	if (ret) {
		pr_err("kasumi: asynchronous I/O bridge unavailable: %d\n", ret);
		goto err_fop_bridge;
	}
	ret = kasumi_file_lock_init();
	if (ret)
		pr_warn("kasumi: source file lock bridge unavailable: %d\n", ret);
	(void)kasumi_fop_override_init();
	(void)kasumi_sop_shadow_init();
	(void)kasumi_dirhijack_init();

	/* On old KMI, the first ingress table published below is the
	 * module-init commit point: bridge-open files may already pin
	 * THIS_MODULE. Keep every later initialization step non-fatal.
	 */
	__module_get(THIS_MODULE);
	WRITE_ONCE(kasumi_unload_pin_held, true);
	(void)kasumi_fake_selinuxfs_access_init();
	(void)kasumi_vfs_view_init();
	ret = kasumi_exec_view_init();
	if (ret)
		pr_warn("kasumi: source exec bridge unavailable: %d\n", ret);
	ret = kasumi_notify_view_init();
	if (ret)
		pr_warn("kasumi: source notification bridge unavailable: %d\n", ret);
	ret = kasumi_rename_view_init();
	if (ret)
		pr_warn("kasumi: source rename bridge unavailable: %d\n", ret);
	ret = kasumi_live_rules_init();
	if (ret)
		pr_warn("kasumi: live rule updates unavailable: %d\n", ret);
	pr_info("kasumi: ready; control via KSU fd\n");
	return 0;

err_fop_bridge:
	kasumi_vnode_aio_exit();
	kasumi_io_notify_exit();
	kasumi_vnode_handle_exit();
	kasumi_fop_bridge_exit();
	kasumi_iop_override_exit();
	kasumi_proc_read_hooks_exit();
	kasumi_fake_mi_exit();
	goto err_buffers;
err_buffers:
	if (kasumi_filldir_cache) {
		kmem_cache_destroy(kasumi_filldir_cache);
		kasumi_filldir_cache = NULL;
	}
	return ret;
}

static void kasumi_stop_views(void)
{
	pr_info("kasumi: shutting down\n");
	kasumi_live_rules_stop();
	mutex_lock(&kasumi_mutation_mutex);
	kasumi_hide_rules_stop();
	mutex_unlock(&kasumi_mutation_mutex);
	kasumi_io_notify_stop();
	kasumi_file_lock_exit();
	kasumi_notify_view_stop();
	kasumi_exec_view_stop();
	kasumi_rename_view_stop();
	kasumi_vfs_view_stop();
	kasumi_vfs_view_drain();

	/*
	 * The path view is served entirely through the VFS lookup/vnode layer
	 * and the proc/mountinfo kprobes; there is no syscall dispatcher or
	 * sys_enter tracepoint to sever.  Tear down handler-reachable state
	 * directly: free the resources those VFS/proc hooks depend on (proc fd
	 * proxies, fake mountinfo, fop/iop shadows, vfs hooks).
	 */
	kasumi_proc_read_hooks_exit();
	kasumi_dirhijack_stop_new();
	kasumi_sop_shadow_stop_new();
	kasumi_dirhijack_exit();
	kasumi_fop_override_stop_new();
	kasumi_fake_selinuxfs_access_stop_new();
	kasumi_fop_bridge_stop_new();
	kasumi_fop_override_exit();
	kasumi_fake_selinuxfs_access_exit();
	kasumi_fop_bridge_exit();
	kasumi_iop_override_exit();
	kasumi_sop_shadow_exit();
	kasumi_fake_mi_exit();
	mutex_lock(&kasumi_config_mutex);
	kasumi_cleanup_locked();
	mutex_unlock(&kasumi_config_mutex);

	kasumi_exec_view_exit();
	kasumi_notify_view_exit();
	kasumi_rename_view_exit();
	kasumi_live_rules_exit();
	kasumi_vfs_view_exit();
	kasumi_store_drain();
	kasumi_vnode_aio_exit();
	kasumi_io_notify_exit();
	kasumi_vnode_handle_exit();
	kasumi_vnode_fops_exit();
	if (kasumi_filldir_cache)
		kmem_cache_destroy(kasumi_filldir_cache);
	pr_info("kasumi: stopped\n");
	if (READ_ONCE(kasumi_unload_pin_held))
		kasumi_bootstrap_release_unload_pin();
}

static DECLARE_RWSEM(kasumi_lifecycle_lock);
static int kasumi_init_error = -EOPNOTSUPP;
static bool kasumi_started;
static bool kasumi_requested;

bool kasumi_is_ready(void)
{
	return smp_load_acquire(&kasumi_init_error) == 0;
}

int kasumi_control_begin(void)
{
	int ret;

	down_read(&kasumi_lifecycle_lock);
	ret = READ_ONCE(kasumi_init_error);
	if (ret)
		up_read(&kasumi_lifecycle_lock);
	return ret;
}

void kasumi_control_end(void)
{
	up_read(&kasumi_lifecycle_lock);
}

static int kasumi_bootstrap_init(void)
{
	int ret;

	down_write(&kasumi_lifecycle_lock);
	if (kasumi_started) {
		ret = 0;
		goto out;
	}
	ret = kasumi_check_builtin_conflicts();
	if (ret)
		goto out;
	ret = ksu_sucompat_module_guard_init();
	if (ret)
		goto out;
	ret = ksu_sucompat_module_guard_acquire();
	if (ret)
		goto unguard;
	ret = ksu_sucompat_ksm_init();
	if (ret)
		goto release_guard;
	ret = kasumi_start_views();
	if (!ret) {
		kasumi_started = true;
		goto out;
	}
	ksu_sucompat_ksm_exit();
release_guard:
	ksu_sucompat_module_guard_release();
unguard:
	ksu_sucompat_module_guard_exit();
out:
	smp_store_release(&kasumi_init_error, ret);
	up_write(&kasumi_lifecycle_lock);
	return ret;
}

static int kasumi_feature_get(u64 *value)
{
	*value = READ_ONCE(kasumi_requested);
	return 0;
}

static int kasumi_feature_set(u64 value)
{
	int ret = 0;

	if (value > 1)
		return -EINVAL;
	if (value) {
		ret = kasumi_bootstrap_init();
		if (ret)
			return ret;
	}
	/* Disabling is for next boot; live views retain the module guard. */
	WRITE_ONCE(kasumi_requested, value != 0);
	return 0;
}

static const struct ksu_feature_handler kasumi_feature = {
    .feature_id = KSU_FEATURE_KASUMI,
    .name = "kasumi",
    .get_handler = kasumi_feature_get,
    .set_handler = kasumi_feature_set,
};

void ksu_kasumi_init(void)
{
	int ret = ksu_register_feature_handler(&kasumi_feature);

	if (ret)
		pr_err("kasumi: feature registration failed: %d\n", ret);
}

void ksu_kasumi_exit(void)
{
	ksu_unregister_feature_handler(KSU_FEATURE_KASUMI);
	down_write(&kasumi_lifecycle_lock);
	if (!kasumi_started) {
		smp_store_release(&kasumi_init_error, -ESHUTDOWN);
		up_write(&kasumi_lifecycle_lock);
		return;
	}
	smp_store_release(&kasumi_init_error, -ESHUTDOWN);
	kasumi_stop_views();
	kasumi_started = false;
	ksu_sucompat_module_guard_release();
	ksu_sucompat_module_guard_exit();
	up_write(&kasumi_lifecycle_lock);
}
