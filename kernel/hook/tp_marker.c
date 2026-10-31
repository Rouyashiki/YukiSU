// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tracepoint marker management: controls which processes have
 * TIF_SYSCALL_TRACEPOINT set so the sys_enter tracepoint fires for them.
 * Extracted from syscall_hook_manager.c for the TSR migration.
 */

#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/printk.h"
#include "selinux/selinux.h"
#include <linux/kprobes.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#include "policy/allowlist.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/tp_marker.h"

// Tracepoint registration count management
// == 1: just us
// >  1: someone else is also using syscall tracepoint e.g. ftrace
static int tracepoint_reg_count = 0;
static DEFINE_SPINLOCK(tracepoint_reg_lock);

void ksu_clear_task_tracepoint_flag_if_needed(struct task_struct *t)
{
	unsigned long flags;
	spin_lock_irqsave(&tracepoint_reg_lock, flags);
	if (tracepoint_reg_count <= 1) {
		ksu_clear_task_tracepoint_flag(t);
	}
	spin_unlock_irqrestore(&tracepoint_reg_lock, flags);
}

// Process marking management
static void handle_process_mark(bool mark)
{
	struct task_struct *p, *t;
	read_lock(&tasklist_lock);
	for_each_process_thread(p, t)
	{
		if (mark)
			ksu_set_task_tracepoint_flag(t);
		else
			ksu_clear_task_tracepoint_flag(t);
	}
	read_unlock(&tasklist_lock);
}

void ksu_mark_all_process(void)
{
	handle_process_mark(true);
}

void ksu_unmark_all_process(void)
{
	handle_process_mark(false);
}

static void ksu_mark_running_process_locked(void)
{
	struct task_struct *p, *t;
	read_lock(&tasklist_lock);
	for_each_process_thread(p, t)
	{
		if (t->pid != 1 && !t->mm) {
			/* Skip kernel threads, but always keep pid 1 markable.
			 */
			continue;
		}
		int uid = task_uid(t).val;
		const struct cred *cred = get_task_cred(t);
		bool ksu_root_process = uid == 0 && is_task_ksu_domain(cred);
		bool is_zygote_process = is_zygote(cred);
		bool is_shell = uid == 2000;
		// before boot completed, we shall mark init for marking zygote
		bool is_init = t->pid == 1;
		if (ksu_root_process || is_zygote_process || is_shell ||
		    is_init || ksu_is_allow_uid(uid)) {
			ksu_set_task_tracepoint_flag(t);
		} else {
			ksu_clear_task_tracepoint_flag(t);
		}
		put_cred(cred);
	}
	read_unlock(&tasklist_lock);
}

void ksu_mark_running_process(void)
{
	unsigned long flags;
	spin_lock_irqsave(&tracepoint_reg_lock, flags);
	if (tracepoint_reg_count <= 1) {
		ksu_mark_running_process_locked();
	}
	spin_unlock_irqrestore(&tracepoint_reg_lock, flags);
}

// Get task mark status
// Returns: 1 if marked, 0 if not marked, -ESRCH if task not found
int ksu_get_task_mark(pid_t pid)
{
	struct task_struct *task;
	int marked = -ESRCH;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task) {
		get_task_struct(task);
		rcu_read_unlock();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		marked =
		    test_task_syscall_work(task, SYSCALL_TRACEPOINT) ? 1 : 0;
#else
		marked =
		    test_tsk_thread_flag(task, TIF_SYSCALL_TRACEPOINT) ? 1 : 0;
#endif // #if LINUX_VERSION_CODE >= KERNEL_VERSIO...
		put_task_struct(task);
	} else {
		rcu_read_unlock();
	}

	return marked;
}

// Set task mark status
// Returns: 0 on success, -ESRCH if task not found
int ksu_set_task_mark(pid_t pid, bool mark)
{
	struct task_struct *task;
	int ret = -ESRCH;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task) {
		get_task_struct(task);
		rcu_read_unlock();
		if (mark) {
			ksu_set_task_tracepoint_flag(task);
		} else {
			ksu_clear_task_tracepoint_flag(task);
		}
		put_task_struct(task);
		ret = 0;
	} else {
		rcu_read_unlock();
	}

	return ret;
}

#ifdef CONFIG_KRETPROBES

static struct kretprobe *init_kretprobe(const char *name,
					kretprobe_handler_t handler)
{
	struct kretprobe *rp = kzalloc(sizeof(struct kretprobe), GFP_KERNEL);
	if (!rp)
		return NULL;
	rp->kp.symbol_name = name;
	rp->handler = handler;
	rp->data_size = 0;
	rp->maxactive = 0;

	int ret = register_kretprobe(rp);
	pr_info("tp_marker: register_%s kretprobe: %d\n", name, ret);
	if (ret) {
		kfree(rp);
		return NULL;
	}

	return rp;
}

static void destroy_kretprobe(struct kretprobe **rp_ptr)
{
	struct kretprobe *rp = *rp_ptr;
	if (!rp)
		return;
	unregister_kretprobe(rp);
	synchronize_rcu();
	kfree(rp);
	*rp_ptr = NULL;
}

static int syscall_regfunc_handler(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	unsigned long flags;
	spin_lock_irqsave(&tracepoint_reg_lock, flags);
	if (tracepoint_reg_count < 1) {
		// while install our tracepoint, mark our processes
		ksu_mark_running_process_locked();
	} else if (tracepoint_reg_count == 1) {
		// while other tracepoint first added, mark all processes
		ksu_mark_all_process();
	}
	tracepoint_reg_count++;
	spin_unlock_irqrestore(&tracepoint_reg_lock, flags);
	return 0;
}

static int syscall_unregfunc_handler(struct kretprobe_instance *ri,
				     struct pt_regs *regs)
{
	unsigned long flags;
	spin_lock_irqsave(&tracepoint_reg_lock, flags);
	tracepoint_reg_count--;
	if (tracepoint_reg_count <= 0) {
		// while no tracepoint left, unmark all processes
		ksu_unmark_all_process();
	} else if (tracepoint_reg_count == 1) {
		// while just our tracepoint left, unmark disallowed processes
		ksu_mark_running_process_locked();
	}
	spin_unlock_irqrestore(&tracepoint_reg_lock, flags);
	return 0;
}

static struct kretprobe *syscall_regfunc_rp = NULL;
static struct kretprobe *syscall_unregfunc_rp = NULL;
#endif /* CONFIG_KRETPROBES */

void ksu_tp_marker_init(void)
{
#ifdef CONFIG_KRETPROBES
	syscall_regfunc_rp =
	    init_kretprobe("syscall_regfunc", syscall_regfunc_handler);
	syscall_unregfunc_rp =
	    init_kretprobe("syscall_unregfunc", syscall_unregfunc_handler);
#endif // #ifdef CONFIG_KRETPROBES

#ifndef CONFIG_KRETPROBES
	ksu_mark_running_process_locked();
#endif // #ifndef CONFIG_KRETPROBES
}

void ksu_tp_marker_exit(void)
{
#ifdef CONFIG_KRETPROBES
	destroy_kretprobe(&syscall_regfunc_rp);
	destroy_kretprobe(&syscall_unregfunc_rp);
#endif // #ifdef CONFIG_KRETPROBES
}
