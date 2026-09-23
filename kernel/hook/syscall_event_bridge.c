// SPDX-License-Identifier: GPL-2.0-only
#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/printk.h"
#include <linux/fcntl.h>
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "policy/allowlist.h"
#include "arch.h"
#include "feature/adb_root.h"
#include "feature/sucompat.h"
#ifdef CONFIG_KSU_YUKIZYGISK
#include "feature/yukizygisk/api.h"
#endif // #ifdef CONFIG_KSU_YUKIZYGISK
#include "hook/setuid_hook.h"
#include "hook/syscall_event_bridge.h"
#include "hook/syscall_hook.h"
#include "hook/tp_marker.h"
#include "klog.h" // IWYU pragma: keep
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "selinux/selinux.h"
#include "sulog/event.h"
#include "preinit/preinit.h"

/*
 * ksud execve hook: controlled by a static key so that the common case
 * (hook disabled after boot) has zero overhead.
 */
DEFINE_STATIC_KEY_TRUE(ksud_execve_key);

void ksu_stop_ksud_execve_hook(void)
{
	static_branch_disable(&ksud_execve_key);
	pr_info("hook_manager: ksud execve hook disabled\n");
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
	if (READ_ONCE(ksu_su_compat_enabled))
		return ksu_handle_stat_sucompat(orig_nr, regs);
	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
	if (READ_ONCE(ksu_su_compat_enabled))
		return ksu_handle_faccessat_sucompat(orig_nr, regs);
	return ksu_syscall_table[orig_nr](regs);
}

// Unmark init's children that are not zygote, stub_zygote, adbd, or ksud.
static void ksu_handle_init_mark_tracker(const char __user *filename_user)
{
	char path[64];
	unsigned long addr;
	const char __user *fn;
	long ret;

	if (unlikely(!filename_user))
		return;

	addr = untagged_addr((unsigned long)filename_user);
	fn = (const char __user *)addr;
	memset(path, 0, sizeof(path));
	ret = strncpy_from_user(path, fn, sizeof(path));
	if (ret < 0)
		return;
	path[sizeof(path) - 1] = '\0';

	if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
		escape_to_root_for_init();
	} else if (likely(strstr(path, "/app_process") == NULL &&
			  strstr(path, "/adbd") == NULL &&
			  strstr(path, "/stub_zygote") == NULL)) {
		ksu_clear_task_tracepoint_flag_if_needed(current);
	}
}

static long __nocfi ksu_hook_execve_common(int orig_nr,
					   const struct pt_regs *regs,
					   bool execveat)
{
	const char __user **filename_user =
	    execveat ? (const char __user **)&PT_REGS_PARM2(regs)
		     : (const char __user **)&PT_REGS_SYSCALL_PARM1(regs);
	const char __user *const __user *argv_user =
	    execveat ? (const char __user *const __user *)PT_REGS_PARM3(regs)
		     : (const char __user *const __user *)PT_REGS_PARM2(regs);
	const struct cred *cred;
	bool current_is_init;
	struct ksu_sulog_pending_event *pending_root_execve = NULL;
	long ret;

	/* ksud boot-time tracking (init second_stage, zygote detection) */
	if (static_branch_unlikely(&ksud_execve_key)) {
		if (execveat)
			ksu_execveat_hook_ksud(regs);
		else
			ksu_execve_hook_ksud(regs);
	}

#ifdef CONFIG_KSU_YUKIZYGISK
	if (execveat)
		ksu_yukizygisk_observe_execveat(regs);
	else
		ksu_yukizygisk_observe_execve(regs);
#endif // #ifdef CONFIG_KSU_YUKIZYGISK

	// AT_EMPTY_PATH execs have no pathname for preinit to replace.
	if (unlikely(current->pid == 1) &&
	    (!execveat || !(PT_REGS_PARM5(regs) & AT_EMPTY_PATH)) &&
	    ksu_preinit_handle_execve(filename_user, argv_user, orig_nr,
				      (struct pt_regs *)regs, &ret))
		return ret;

	if (current_euid().val == 0)
		pending_root_execve = ksu_sulog_capture_root_execve(
		    *filename_user, argv_user, GFP_KERNEL);

	cred = current_cred();
	current_is_init = is_init(cred);

	if (current->pid != 1 && current_is_init) {
		ksu_handle_init_mark_tracker(*filename_user);
		ret = execveat
			  ? ksu_adb_root_handle_execveat((struct pt_regs *)regs)
			  : ksu_adb_root_handle_execve((struct pt_regs *)regs);
		if (ret) {
			pr_err("hook_manager: adb root failed: %ld\n", ret);
		}
		ret = ksu_syscall_table[orig_nr](regs);
		ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
		return ret;
	} else if (ksu_sucompat_exec_enabled()) {
		ret = execveat ? ksu_handle_execveat_sucompat(filename_user,
							      orig_nr, regs)
			       : ksu_handle_execve_sucompat(filename_user,
							    orig_nr, regs);
		ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
		return ret;
	}

	ret = ksu_syscall_table[orig_nr](regs);
	ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
	return ret;
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	return ksu_hook_execve_common(orig_nr, regs, false);
}

long __nocfi ksu_hook_execveat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_hook_execve_common(orig_nr, regs, true);
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
	long ret;
	uid_t old_uid = current_uid().val;

	/* Call the original syscall first, then inspect the result */
	ret = ksu_syscall_table[orig_nr](regs);
	if (ret < 0)
		return ret;

	ksu_handle_setresuid(old_uid, current_uid().val);

	return ret;
}
