#include <asm/current.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/input-event-codes.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#include "infra/hook_lifetime.h"
#include "policy/allowlist.h"
#include "arch.h"
#include "feature/selinux_hide.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "klog.h" // IWYU pragma: keep
#include "ksu.h"
#include "runtime/ksud_boot.h"
#include "runtime/ksud.h"
#include "policy/feature.h"
#include "selinux/selinux.h"
#include "manager/throne_tracker.h"
#include "preinit/preinit.h"

static const char KERNEL_SU_RC[] =
    "\n"

    "on post-fs-data\n"
    "    start logd\n"
    // We should wait for the post-fs-data finish
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " post-fs-data\n"
    "\n"

    "on nonencrypted\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"

    "on property:vold.decrypt=trigger_restart_framework\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH " services\n"
    "\n"

    "on property:sys.boot_completed=1\n"
    "    exec u:r:" KERNEL_SU_DOMAIN ":s0 root -- " KSUD_PATH
    " boot-completed\n"
    "\n"

    "\n";

static void stop_init_rc_hook(void);
static void stop_input_hook(void);

static struct work_struct stop_init_rc_hook_work;
static struct work_struct stop_input_hook_work;
static DEFINE_MUTEX(input_hook_lock);
static bool input_hook_registered;

static void ksu_initialize_selinux(void)
{
	ksu_preinit_release();
	ksu_selinux_hide_handle_second_stage();
	apply_kernelsu_rules();
	cache_sid();
	setup_ksu_cred();
	ksu_feature_apply_forced();
}

static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
	const char __user *native;

	if (get_user(native, argv.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

static int count(struct user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.native != NULL) {
		for (;;) {
			const char __user *p = get_user_arg_ptr(argv, i);

			if (!p)
				break;
			if (IS_ERR(p))
				return -EFAULT;
			if (i >= max)
				return -E2BIG;
			++i;

			if (fatal_signal_pending(current))
				return -ERESTARTNOHAND;
		}
	}

	return i;
}

static bool check_argv(struct user_arg_ptr argv, int index,
		       const char *expected, char *buf, size_t buf_len)
{
	const char __user *p;
	int argc;

	argc = count(argv, MAX_ARG_STRINGS);
	if (argc <= index)
		return false;

	p = get_user_arg_ptr(argv, index);
	if (!p || IS_ERR(p))
		goto fail;

	if (strncpy_from_user(buf, p, buf_len) <= 0)
		goto fail;

	buf[buf_len - 1] = '\0';
	return !strcmp(buf, expected);

fail:
	pr_err("check_argv failed\n");
	return false;
}

/*
 * IMPORTANT NOTE: the TSR execve path still cannot rely on envp/flags on some
 * GKI kernels, so callers may legitimately pass NULL for those arguments.
 */
void ksu_handle_execveat_ksud(const char *filename, struct user_arg_ptr *argv,
			      struct user_arg_ptr *envp, int *flags)
{
	static const char app_process[] = "/system/bin/app_process";
	static bool first_zygote = true;
	static const char system_bin_init[] = "/system/bin/init";
	static const char old_system_init[] = "/init";
	static bool init_second_stage_executed = false;

	(void)flags;

	if (!filename)
		return;

	/* This applies to versions Android 10+ */
	if (unlikely(!memcmp(filename, system_bin_init,
			     sizeof(system_bin_init) - 1) &&
		     argv)) {
		char buf[16];

		if (!init_second_stage_executed &&
		    check_argv(*argv, 1, "second_stage", buf, sizeof(buf))) {
			pr_info("/system/bin/init second_stage executed via "
				"argv1 check\n");
			ksu_initialize_selinux();
			init_second_stage_executed = true;
		}
	} else if (unlikely(!memcmp(filename, old_system_init,
				    sizeof(old_system_init) - 1) &&
			    argv)) {
		/* This applies to versions between Android 6 ~ 9. */
		int argc = count(*argv, MAX_ARG_STRINGS);

		pr_info("/init argc: %d\n", argc);
		if (argc > 1 && !init_second_stage_executed) {
			char buf[16];

			if (check_argv(*argv, 1, "--second-stage", buf,
				       sizeof(buf))) {
				pr_info("/init second_stage executed via argv1 "
					"check\n");
				ksu_initialize_selinux();
				init_second_stage_executed = true;
			}
		} else if (argc == 1 && !init_second_stage_executed && envp) {
			int envc = count(*envp, MAX_ARG_STRINGS);
			int n;

			for (n = 1; n <= envc; n++) {
				const char __user *p =
				    get_user_arg_ptr(*envp, n);
				char env[256];
				char *env_name;
				char *env_value;

				if (!p || IS_ERR(p))
					continue;
				if (strncpy_from_user(env, p, sizeof(env)) < 0)
					continue;

				env_name = env;
				env_value = strchr(env, '=');
				if (!env_value)
					continue;
				*env_value = '\0';
				env_value++;

				if (!strcmp(env_name, "INIT_SECOND_STAGE") &&
				    (!strcmp(env_value, "1") ||
				     !strcmp(env_value, "true"))) {
					pr_info("/init second_stage executed "
						"via envp check\n");
					ksu_initialize_selinux();
					init_second_stage_executed = true;
					break;
				}
			}
		}
	}

	if (unlikely(first_zygote &&
		     !memcmp(filename, app_process, sizeof(app_process) - 1) &&
		     argv)) {
		char buf[16];

		if (check_argv(*argv, 1, "-Xzygote", buf, sizeof(buf))) {
			pr_info(
			    "exec zygote, /data prepared, second_stage: %d\n",
			    init_second_stage_executed);
			ksu_preinit_release();
			on_post_fs_data();
			first_zygote = false;
			ksu_stop_ksud_execve_hook();
		}
	}
}

static void
ksu_execve_hook_ksud_common(const char __user *filename_user,
			    const char __user *const __user *argv_user)
{
	struct user_arg_ptr argv = {
	    .native = argv_user,
	};
	char path[32];
	const char __user *fn;
	long ret;
	unsigned long addr;

	if (!filename_user)
		return;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;

	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0) {
		pr_err("Access filename failed for execve_handler_pre\n");
		return;
	}

	ksu_handle_execveat_ksud(path, &argv, NULL, NULL);
}

void ksu_execve_hook_ksud(const struct pt_regs *regs)
{
	ksu_execve_hook_ksud_common(
	    (const char __user *)PT_REGS_PARM1(regs),
	    (const char __user *const __user *)PT_REGS_PARM2(regs));
}

void ksu_execveat_hook_ksud(const struct pt_regs *regs)
{
	ksu_execve_hook_ksud_common(
	    (const char __user *)PT_REGS_PARM2(regs),
	    (const char __user *const __user *)PT_REGS_PARM3(regs));
}

// ---------------------------------------------------------------
// init.rc injection via sys_read/sys_fstat syscall table hooks
// ---------------------------------------------------------------

static ssize_t (*orig_read)(struct file *, char __user *, size_t, loff_t *);
static ssize_t (*orig_read_iter)(struct kiocb *, struct iov_iter *);
static struct file_operations fops_proxy;
static DEFINE_MUTEX(init_rc_fops_lock);
static ssize_t ksu_rc_pos = 0;
const size_t ksu_rc_len = sizeof(KERNEL_SU_RC) - 1;

#define MODULE_RC_PATH_VOLD "/metadata/vold/ksu/modules.rc"
#define MODULE_RC_PATH_WATCHDOG "/metadata/watchdog/ksu/modules.rc"
#define MODULE_RC_PATH_DEFAULT "/metadata/ksu/modules.rc"

static char *module_rc_buf;
static size_t module_rc_len;
static ssize_t module_rc_pos;

static struct file *open_module_rc(const char **chosen_path)
{
	struct file *f = filp_open(MODULE_RC_PATH_VOLD, O_RDONLY, 0);

	if (!IS_ERR(f)) {
		*chosen_path = MODULE_RC_PATH_VOLD;
		return f;
	}
	f = filp_open(MODULE_RC_PATH_WATCHDOG, O_RDONLY, 0);

	if (!IS_ERR(f)) {
		*chosen_path = MODULE_RC_PATH_WATCHDOG;
		return f;
	}

	f = filp_open(MODULE_RC_PATH_DEFAULT, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		*chosen_path = MODULE_RC_PATH_DEFAULT;
		return f;
	}

	*chosen_path = MODULE_RC_PATH_DEFAULT;
	return f;
}

static void load_module_rc_once(void)
{
	static bool loaded = false;
	struct file *f;
	const char *path = NULL;
	loff_t pos = 0;
	ssize_t ret;
	size_t file_size;
	const struct cred *old_cred;

	if (loaded)
		return;
	loaded = true;

	if (ksu_no_custom_rc) {
		pr_info("module rc: custom rc is disabled\n");
		return;
	}

	old_cred = ksu_cred ? override_creds(ksu_cred) : NULL;

	f = open_module_rc(&path);
	if (IS_ERR(f)) {
		pr_info("module rc: open %s failed: %ld\n", path, PTR_ERR(f));
		goto out_revert_creds;
	}

	if (!S_ISREG(file_inode(f)->i_mode)) {
		pr_warn("module rc: %s is not a regular file\n", path);
		goto out_close_file;
	}

	file_size = i_size_read(file_inode(f));
	if (!file_size) {
		pr_warn("module rc: skip empty module rc\n");
		goto out_close_file;
	}

	module_rc_buf = kvmalloc(file_size, GFP_KERNEL);
	if (!module_rc_buf) {
		pr_err("module rc: alloc %zu failed\n", file_size);
		goto out_close_file;
	}

	ret = kernel_read(f, module_rc_buf, file_size, &pos);
	if (ret <= 0) {
		pr_err("module rc: read failed: %zd\n", ret);
		kvfree(module_rc_buf);
		module_rc_buf = NULL;
		goto out_close_file;
	}

	module_rc_len = ret;
	pr_info("module rc: loaded %zu bytes from %s\n", module_rc_len, path);

out_close_file:
	filp_close(f, NULL);

out_revert_creds:
	if (old_cred)
		revert_creds(old_cred);
}

static void free_module_rc(void)
{
	kvfree(module_rc_buf);
	module_rc_buf = NULL;
	module_rc_len = 0;
}

static ssize_t read_proxy(struct file *file, char __user *buf, size_t count,
			  loff_t *pos)
{
	ssize_t ret = 0;
	size_t append_count;
	if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
		goto append_ksu_rc;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos < module_rc_len)
		goto append_module_rc;

	ret = orig_read(file, buf, count, pos);
	if (ret != 0)
		return ret;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos >= module_rc_len)
		return ret;

	pr_info("read_proxy: orig read finished, start append rc\n");

append_ksu_rc:
	if (ksu_rc_pos < ksu_rc_len) {
		append_count = ksu_rc_len - ksu_rc_pos;
		if (append_count > count - ret)
			append_count = count - ret;
		if (copy_to_user(buf + ret, KERNEL_SU_RC + ksu_rc_pos,
				 append_count)) {
			pr_info(
			    "read_proxy: append error, totally appended %ld\n",
			    ksu_rc_pos);
			return ret;
		}
		pr_info("read_proxy: append static %zu\n", append_count);
		ksu_rc_pos += append_count;
		ret += append_count;
		if (ksu_rc_pos == ksu_rc_len)
			pr_info("read_proxy: static append done\n");
	}

append_module_rc:
	if (module_rc_pos < module_rc_len && (size_t)ret < count) {
		append_count = module_rc_len - module_rc_pos;
		if (append_count > count - ret)
			append_count = count - ret;
		if (copy_to_user(buf + ret, module_rc_buf + module_rc_pos,
				 append_count)) {
			pr_info("read_proxy: module append error, totally "
				"appended %zd\n",
				module_rc_pos);
			return ret;
		}
		pr_info("read_proxy: append module %zu\n", append_count);
		module_rc_pos += append_count;
		ret += append_count;
		if (module_rc_pos == (ssize_t)module_rc_len) {
			pr_info("read_proxy: module append done\n");
			free_module_rc();
		}
	}

	return ret;
}

static ssize_t read_iter_proxy(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t ret = 0;
	size_t append_count;
	if (ksu_rc_pos && ksu_rc_pos < ksu_rc_len)
		goto append_ksu_rc;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos < module_rc_len)
		goto append_module_rc;

	ret = orig_read_iter(iocb, to);
	if (ret != 0)
		return ret;
	if (ksu_rc_pos >= ksu_rc_len && module_rc_pos >= module_rc_len)
		return ret;

	pr_info("read_iter_proxy: orig read finished, start append rc\n");

append_ksu_rc:
	if (ksu_rc_pos < ksu_rc_len) {
		append_count = copy_to_iter(KERNEL_SU_RC + ksu_rc_pos,
					    ksu_rc_len - ksu_rc_pos, to);
		if (!append_count) {
			pr_info("read_iter_proxy: append error, totally "
				"appended %ld\n",
				ksu_rc_pos);
			return ret;
		}
		pr_info("read_iter_proxy: append static %zu\n", append_count);
		ksu_rc_pos += append_count;
		ret += append_count;
		if (ksu_rc_pos == ksu_rc_len)
			pr_info("read_iter_proxy: static append done\n");
	}

append_module_rc:
	if (module_rc_pos < module_rc_len) {
		append_count = copy_to_iter(module_rc_buf + module_rc_pos,
					    module_rc_len - module_rc_pos, to);
		if (!append_count) {
			pr_info("read_iter_proxy: module append error, "
				"appended %zd\n",
				module_rc_pos);
			return ret;
		}
		pr_info("read_iter_proxy: append module %zu\n", append_count);
		module_rc_pos += append_count;
		ret += append_count;
		if (module_rc_pos == (ssize_t)module_rc_len) {
			pr_info("read_iter_proxy: module append done\n");
			free_module_rc();
		}
	}
	return ret;
}

static bool is_init_rc(struct file *fp)
{
	if (strcmp(current->comm, "init")) {
		return false;
	}

	if (!d_is_reg(fp->f_path.dentry)) {
		return false;
	}

	// Bind mounts retain the source dentry name, so match the mounted path.
	char path[256];
	char *dpath = d_path(&fp->f_path, path, sizeof(path));

	if (IS_ERR(dpath)) {
		return false;
	}

	if (strcmp(dpath, "/system/etc/init/hw/init.rc")) {
		return false;
	}

	return true;
}

static void ksu_handle_sys_read(unsigned int fd)
{
	static bool rc_hooked;
	const struct file_operations *ops;
	struct file *file = fget(fd);
	if (!file) {
		return;
	}

	if (!is_init_rc(file)) {
		goto skip;
	}

	mutex_lock(&init_rc_fops_lock);
	if (rc_hooked) {
		stop_init_rc_hook();
		goto unlock;
	}

	memcpy(&fops_proxy, file->f_op, sizeof(struct file_operations));
	fops_proxy.owner = THIS_MODULE;
	orig_read = file->f_op->read;
	if (orig_read) {
		fops_proxy.read = read_proxy;
	}
	orig_read_iter = file->f_op->read_iter;
	if (orig_read_iter) {
		fops_proxy.read_iter = read_iter_proxy;
	}
	ops = fops_get(&fops_proxy);
	if (!ops)
		goto unlock;

	load_module_rc_once();
	pr_info("read init.rc, comm: %s, rc_count: %zu, module_rc: %zu\n",
		current->comm, ksu_rc_len, module_rc_len);
	replace_fops(file, ops);
	rc_hooked = true;

unlock:
	mutex_unlock(&init_rc_fops_lock);
skip:
	fput(file);
}

// ---------------------------------------------------------------
// Syscall table hooks for init.rc injection (sys_read + sys_fstat)
// ---------------------------------------------------------------

static syscall_fn_t orig_sys_read;
static syscall_fn_t orig_sys_fstat;

static long __nocfi ksu_sys_read_impl(const struct pt_regs *regs)
{
	unsigned int fd = PT_REGS_PARM1(regs);

	ksu_handle_sys_read(fd);
	return orig_sys_read(regs);
}

static long __nocfi ksu_sys_read(const struct pt_regs *regs)
{
	int index = ksu_hook_enter(KSU_HOOK_INITRC);
	long ret = ksu_sys_read_impl(regs);

	ksu_hook_leave(KSU_HOOK_INITRC, index);
	return ret;
}

static long __nocfi ksu_sys_fstat_impl(const struct pt_regs *regs)
{
	unsigned int fd = PT_REGS_PARM1(regs);
	long ret;

	ret = orig_sys_fstat(regs);
	if (ret)
		return ret;

	struct file *file = fget(fd);
	if (!file)
		return ret;

	if (is_init_rc(file)) {
		void __user *statbuf =
		    (void __user *)(unsigned long)PT_REGS_PARM2(regs);
		void __user *st_size_ptr =
		    statbuf + offsetof(struct stat, st_size);
		long size, new_size;
		size_t extra;
		load_module_rc_once();
		extra = ksu_rc_len + module_rc_len;
		if (!copy_from_user_nofault(&size, st_size_ptr, sizeof(long))) {
			new_size = size + extra;
			pr_info(
			    "adding rc len: %ld -> %ld (static=%zu module=%zu)",
			    size, new_size, ksu_rc_len, module_rc_len);
			copy_to_user_nofault(st_size_ptr, &new_size,
					     sizeof(long));
		}
	}
	fput(file);
	return ret;
}

static long __nocfi ksu_sys_fstat(const struct pt_regs *regs)
{
	int index = ksu_hook_enter(KSU_HOOK_INITRC);
	long ret = ksu_sys_fstat_impl(regs);

	ksu_hook_leave(KSU_HOOK_INITRC, index);
	return ret;
}

// ---------------------------------------------------------------
// Input event hook (kept as kprobe — independent of syscall hooks)
// ---------------------------------------------------------------

static unsigned int volumedown_pressed_count = 0;

static bool is_volumedown_enough(unsigned int count)
{
	return count >= 3;
}

static int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code,
					 int *value)
{
	if (*type == EV_KEY && *code == KEY_VOLUMEDOWN) {
		int val = *value;
		if (val) {
			volumedown_pressed_count += 1;
			if (is_volumedown_enough(volumedown_pressed_count)) {
				stop_input_hook();
			}
		}
	}

	return 0;
}

bool ksu_is_safe_mode(void)
{
	static bool safe_mode = false;
	if (safe_mode) {
		return true;
	}

	if (ksu_late_loaded) {
		return false;
	}

	stop_input_hook();

	pr_info("volumedown_pressed_count: %d\n", volumedown_pressed_count);
	if (is_volumedown_enough(volumedown_pressed_count)) {
		pr_info(
		    "KEY_VOLUMEDOWN pressed max times, safe mode detected!\n");
		safe_mode = true;
		return true;
	}

	return false;
}

// ---------------------------------------------------------------
// Input event kprobe (kept as-is)
// ---------------------------------------------------------------

static int input_handle_event_handler_pre(struct kprobe *p,
					  struct pt_regs *regs)
{
	unsigned int *type = (unsigned int *)&PT_REGS_PARM2(regs);
	unsigned int *code = (unsigned int *)&PT_REGS_PARM3(regs);
	int *value = (int *)&PT_REGS_CCALL_PARM4(regs);
	return ksu_handle_input_handle_event(type, code, value);
}

static struct kprobe input_event_kp = {
    .symbol_name = "input_event",
    .pre_handler = input_handle_event_handler_pre,
};

// ---------------------------------------------------------------
// Hook stop helpers
// ---------------------------------------------------------------

static void do_stop_init_rc_hook(struct work_struct *work)
{
	ksu_syscall_table_unhook(__NR_read);
	ksu_syscall_table_unhook(__NR_fstat);
	pr_info("ksud: init_rc syscall table hooks removed\n");
}

static void do_stop_input_hook(struct work_struct *work)
{
	mutex_lock(&input_hook_lock);
	if (input_hook_registered) {
		unregister_kprobe(&input_event_kp);
		input_hook_registered = false;
	}
	mutex_unlock(&input_hook_lock);
}

static void stop_init_rc_hook(void)
{
	bool ret = schedule_work(&stop_init_rc_hook_work);
	pr_info("unregister init_rc_hook: %d!\n", ret);
}

static void stop_input_hook(void)
{
	static bool input_hook_stopped = false;

	if (input_hook_stopped) {
		return;
	}
	input_hook_stopped = true;
	schedule_work(&stop_input_hook_work);
}

void ksu_stop_input_hook_runtime(void)
{
	stop_input_hook();
}

// ---------------------------------------------------------------
// ksud: module support
// ---------------------------------------------------------------

void ksu_ksud_init(void)
{
	int ret;

	INIT_WORK(&stop_init_rc_hook_work, do_stop_init_rc_hook);
	INIT_WORK(&stop_input_hook_work, do_stop_input_hook);

	/* Install syscall table hooks for init.rc injection */
	ret = ksu_syscall_table_hook(__NR_read, ksu_sys_read, &orig_sys_read);
	pr_info("ksud: sys_read table hook: %d\n", ret);

	ret =
	    ksu_syscall_table_hook(__NR_fstat, ksu_sys_fstat, &orig_sys_fstat);
	pr_info("ksud: sys_fstat table hook: %d\n", ret);

	/* Input event kprobe (for safe mode detection) */
	mutex_lock(&input_hook_lock);
	ret = register_kprobe(&input_event_kp);
	input_hook_registered = !ret;
	mutex_unlock(&input_hook_lock);
	pr_info("ksud: input_event_kp: %d\n", ret);
}

void ksu_ksud_exit(void)
{
	/* Syscall table hooks are cleaned up by ksu_syscall_hook_exit(). */
	do_stop_input_hook(NULL);
	cancel_work_sync(&stop_input_hook_work);
	cancel_work_sync(&stop_init_rc_hook_work);
}
