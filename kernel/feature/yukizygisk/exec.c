#include <linux/binfmts.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "api.h"
#include "arch.h"
#include "internal.h"
#include "tango.h"
#include "hook/lsm_hook.h"
#include "policy/feature.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep
#include "uapi/yukizygisk.h"

static const char app_process[] = "app_process";

#define YZ_ENABLE_LSM_INJECTOR 1

/* Linux 6.12 made the hook argument const. */
#define YZ_BPRM_HOOK_TARGET "selinux_bprm_committed_creds"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define YZ_BPRM_HOOK_CONST 1
#define YZ_BPRM_HOOK_ABI "const struct linux_binprm *"
#else
#define YZ_BPRM_HOOK_CONST 0
#define YZ_BPRM_HOOK_ABI "struct linux_binprm *"
#endif

#if YZ_BPRM_HOOK_CONST
typedef const struct linux_binprm yz_bprm_arg_t;
#else
typedef struct linux_binprm yz_bprm_arg_t;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define YZ_BPRM_HOOK_CFI "kcfi"
#else
#define YZ_BPRM_HOOK_CFI "clang-cfi/.cfi_jt"
#endif

static void yz_bprm_committed_creds(yz_bprm_arg_t *bprm);
static struct ksu_lsm_hook yz_exec_hook = KSU_LSM_HOOK_INIT(
    bprm_committed_creds, YZ_BPRM_HOOK_TARGET, yz_bprm_committed_creds, 0);
static bool yz_exec_hook_registered;

typedef void (*bprm_committed_creds_fn)(yz_bprm_arg_t *bprm);

struct yz_user_arg_ptr {
	const char __user *const __user *native;
};

static const char __user *yz_get_user_arg_ptr(struct yz_user_arg_ptr argv,
					      int nr)
{
	const char __user *native;

	if (get_user(native, argv.native + nr))
		return ERR_PTR(-EFAULT);

	return native;
}

static int yz_count_user_args(struct yz_user_arg_ptr argv, int max)
{
	int i = 0;

	if (argv.native != NULL) {
		for (;;) {
			const char __user *p = yz_get_user_arg_ptr(argv, i);

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

static void yz_copy_karg_name(char *dst, size_t dst_len, const char *src)
{
	size_t i;

	if (!dst_len)
		return;
	for (i = 0; i + 1 < dst_len && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static bool yz_copy_argv_arg(struct yz_user_arg_ptr argv, int index, char *buf,
			     size_t buf_len)
{
	const char __user *p;

	if (!buf_len)
		return false;

	p = yz_get_user_arg_ptr(argv, index);
	if (!p || IS_ERR(p))
		return false;

	if (strncpy_from_user(buf, p, buf_len) <= 0)
		return false;
	buf[buf_len - 1] = '\0';
	return true;
}

static bool yz_parse_exec_argv(struct yz_user_arg_ptr argv, char *socket_name,
			       size_t socket_name_len)
{
	int argc = yz_count_user_args(argv, MAX_ARG_STRINGS);
	bool found = false;
	int i;

	if (socket_name_len)
		socket_name[0] = '\0';
	if (argc <= 0)
		return false;

	for (i = 0; i < argc && i < 64; i++) {
		static const char socket_prefix[] = "--socket-name=";
		char arg[96];

		if (!yz_copy_argv_arg(argv, i, arg, sizeof(arg)))
			continue;
		if (!strcmp(arg, "-Xzygote"))
			found = true;
		else if (!strncmp(arg, socket_prefix,
				  sizeof(socket_prefix) - 1))
			yz_copy_karg_name(socket_name, socket_name_len,
					  arg + sizeof(socket_prefix) - 1);
	}

	if (found && socket_name_len && socket_name[0] == '\0')
		yz_copy_karg_name(socket_name, socket_name_len, "zygote");
	return found;
}

static void
ksu_yukizygisk_observe_exec(const char __user *filename_user,
			    const char __user *const __user *argv_user)
{
	struct yz_user_arg_ptr argv = {
	    .native = argv_user,
	};
	char path[64];
	char socket_name[32];
	const char __user *fn;
	unsigned long addr;
	long ret;

	if (!READ_ONCE(yukizygisk_enabled)) {
		yz_feature_enable_early();
		return;
	}

	if (!filename_user)
		return;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;

	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return;
	path[sizeof(path) - 1] = '\0';

	if (!strstr(path, "/app_process"))
		return;

	if (yz_parse_exec_argv(argv, socket_name, sizeof(socket_name)))
		pr_info("yukizygisk: zygote exec detected pid=%d socket=%s "
			"comm=%s file=%s\n",
			current->pid, socket_name, current->comm, path);
}

void ksu_yukizygisk_observe_execve(const struct pt_regs *regs)
{
	ksu_yukizygisk_observe_exec(
	    (const char __user *)PT_REGS_PARM1(regs),
	    (const char __user *const __user *)PT_REGS_PARM2(regs));
}

void ksu_yukizygisk_observe_execveat(const struct pt_regs *regs)
{
	ksu_yukizygisk_observe_exec(
	    (const char __user *)PT_REGS_PARM2(regs),
	    (const char __user *const __user *)PT_REGS_PARM3(regs));
}

static bool yz_is_app_process_path(const char *filename)
{
	const char *base;

	if (!filename)
		return false;
	base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	return !strncmp(base, app_process, sizeof(app_process) - 1);
}

static bool yz_is_init_child(void)
{
	struct task_struct *parent;
	bool is_init_child;

	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	is_init_child = parent && task_pid_nr(parent) == 1;
	rcu_read_unlock();
	return is_init_child;
}

static void __nocfi yz_bprm_committed_creds(yz_bprm_arg_t *bprm)
{
	const char *filename = bprm ? bprm->filename : NULL;
	char native_label[YZ_NATIVE_TARGET_VALUE_MAX] = {};
	u8 native_target_type = 0;
	bool early_native = false;
	bool live_enabled;
	bool early_enabled;
	bool by_sid;
	bool by_path;
	bool by_native;
	bool live_native;

	((bprm_committed_creds_fn)yz_exec_hook.original)(bprm);
	live_enabled = READ_ONCE(yukizygisk_enabled);
	early_enabled = yz_early_native_active();
	if (unlikely(!live_enabled && !early_enabled))
		return;
	by_sid = live_enabled && is_zygote(current_cred());
	by_path = live_enabled && yz_is_app_process_path(filename);
	live_native = live_enabled && !by_path &&
		      yz_match_live_native_target(filename, native_label,
						  sizeof(native_label),
						  &native_target_type);
	by_native = live_native && yz_is_init_child();
	if (!by_path && !by_native && early_enabled &&
	    yz_match_early_native_target(filename, native_label,
					 sizeof(native_label),
					 &native_target_type)) {
		by_native = true;
		early_native = true;
	}
	if (unlikely(by_sid || by_path || by_native)) {
		if (by_native)
			pr_info("yukizygisk: native target exec pid=%d tgid=%d "
				"file=%s target=%s early=%d\n",
				current->pid, current->tgid,
				filename ?: "(null)", native_label,
				early_native ? 1 : 0);

		pr_debug("yukizygisk: exec match pid=%d tgid=%d file=%s sid=%d "
			 "path=%d native=%d\n",
			 current->pid, current->tgid, filename ?: "(null)",
			 by_sid, by_path, by_native);

		if (by_path || by_native)
			yz_schedule_injection(by_native, native_target_type,
					      early_native, native_label);
	}
}

void yz_exec_init(void)
{
	if (ksu_register_feature_handler(&yukizygisk_feature_handler))
		pr_err("yukizygisk: feature handler registration failed\n");
	else
		pr_info("yukizygisk: feature handler registered\n");
}

int yz_exec_enable(void)
{
#if YZ_ENABLE_LSM_INJECTOR
	int ret;

	if (yz_exec_hook_registered)
		return 0;

	ret = ksu_register_lsm_hook(&yz_exec_hook);
	if (ret) {
		pr_err("yukizygisk: exec injection hook registration failed "
		       "err=%d\n",
		       ret);
		return ret;
	}
	yz_exec_hook_registered = true;
	ret = yz_tango_enable();
	if (ret)
		pr_warn("yukizygisk: Tango hook unavailable err=%d\n", ret);
	pr_info("yukizygisk: exec injection hook registered abi=%s "
		"resolver=%s\n",
		YZ_BPRM_HOOK_ABI, YZ_BPRM_HOOK_CFI);
	return 0;
#else
	pr_err("yukizygisk: exec injection disabled at build time\n");
	return -EOPNOTSUPP;
#endif
}

void yz_exec_disable(void)
{
	yz_tango_disable();
#if YZ_ENABLE_LSM_INJECTOR
	if (yz_exec_hook_registered) {
		ksu_unregister_lsm_hook(&yz_exec_hook);
		yz_exec_hook_registered = false;
	}
#endif
}

void yz_exec_exit(void)
{
	yz_exec_disable();
	yz_cleanup_module_policies();
	ksu_unregister_feature_handler(KSU_FEATURE_YUKIZYGISK);
}
