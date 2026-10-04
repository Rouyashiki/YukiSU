#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/hashtable.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

#include <trace/events/sched.h>

#include "api.h"
#include "internal.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

enum yz_lifecycle_state {
	YZ_LIFECYCLE_FORKED,
	YZ_LIFECYCLE_SPECIALIZED,
	YZ_LIFECYCLE_EXITED,
};

struct yz_lifecycle_child {
	struct hlist_node pid_node;
	pid_t pid; /* tgid of the app process; 0 == free slot */
	uid_t uid;
	u64 start_boottime;
	enum yz_lifecycle_state state;
};

#define YZ_LIFECYCLE_MAX_CHILDREN 512
static struct yz_lifecycle_child
    yz_lifecycle_children[YZ_LIFECYCLE_MAX_CHILDREN];
static DEFINE_HASHTABLE(yz_lifecycle_pids, 9);
static DEFINE_SPINLOCK(yz_lifecycle_lock);

static void yz_lifecycle_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_lifecycle_lock, flags);
	memset(yz_lifecycle_children, 0, sizeof(yz_lifecycle_children));
	hash_init(yz_lifecycle_pids);
	spin_unlock_irqrestore(&yz_lifecycle_lock, flags);
}

/* yz_lifecycle_lock must be held. */
static int yz_lifecycle_slot_of(pid_t pid)
{
	struct yz_lifecycle_child *child;
	int i;

	if (!pid) {
		for (i = 0; i < YZ_LIFECYCLE_MAX_CHILDREN; i++)
			if (!yz_lifecycle_children[i].pid)
				return i;
		return -1;
	}
	hash_for_each_possible(yz_lifecycle_pids, child, pid_node, pid)
	{
		if (child->pid == pid)
			return child - yz_lifecycle_children;
	}
	return -1;
}

static void yz_lifecycle_track(struct task_struct *task)
{
	pid_t pid = task->tgid;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&yz_lifecycle_lock, flags);
	i = yz_lifecycle_slot_of(pid);
	if (i < 0)
		i = yz_lifecycle_slot_of(0);
	if (i >= 0 &&
	    (!yz_lifecycle_children[i].pid ||
	     yz_lifecycle_children[i].start_boottime != task->start_boottime)) {
		hash_del(&yz_lifecycle_children[i].pid_node);
		yz_lifecycle_children[i].pid = pid;
		yz_lifecycle_children[i].uid = (uid_t)-1;
		yz_lifecycle_children[i].start_boottime = task->start_boottime;
		yz_lifecycle_children[i].state = YZ_LIFECYCLE_FORKED;
		hash_add(yz_lifecycle_pids, &yz_lifecycle_children[i].pid_node,
			 pid);
	}
	spin_unlock_irqrestore(&yz_lifecycle_lock, flags);
}

void yz_lifecycle_on_exit(struct task_struct *task)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&yz_lifecycle_lock, flags);
	i = yz_lifecycle_slot_of(task->tgid);
	if (i >= 0 && yz_lifecycle_children[i].start_boottime ==
			  READ_ONCE(task->group_leader->start_boottime))
		yz_lifecycle_children[i].state = YZ_LIFECYCLE_EXITED;
	spin_unlock_irqrestore(&yz_lifecycle_lock, flags);
	yz_fd_handoff_release(task);
}

#ifdef CONFIG_TRACEPOINTS

static bool yz_lifecycle_fork_registered;
static bool yz_lifecycle_free_registered;

static void yz_lifecycle_on_fork(void *data, struct task_struct *parent,
				 struct task_struct *child)
{
	bool from_zygote;

	(void)data;
	if (!READ_ONCE(yukizygisk_enabled))
		return;

	/* Track process leaders, not zygote worker threads. */
	if (child->pid != child->tgid)
		return;

	rcu_read_lock();
	from_zygote = is_zygote(__task_cred(parent));
	rcu_read_unlock();
	if (!from_zygote)
		return;

	yz_lifecycle_track(child);
	pr_info("yukizygisk: app forked pid=%d zygote=%d\n", child->pid,
		parent->pid);
}

static void yz_lifecycle_on_free(void *data, struct task_struct *p)
{
	unsigned long flags;
	bool tracked = false;
	uid_t uid = 0;
	int i;

	(void)data;
	if (!READ_ONCE(yukizygisk_enabled))
		return;

	if (p->pid != p->tgid)
		return;

	spin_lock_irqsave(&yz_lifecycle_lock, flags);
	i = yz_lifecycle_slot_of(p->pid);
	if (i >= 0 && yz_lifecycle_children[i].start_boottime ==
			  READ_ONCE(p->start_boottime)) {
		tracked = true;
		uid = yz_lifecycle_children[i].uid;
		hash_del(&yz_lifecycle_children[i].pid_node);
		yz_lifecycle_children[i].pid = 0;
	}
	spin_unlock_irqrestore(&yz_lifecycle_lock, flags);

	if (tracked) {
		pr_info("yukizygisk: app exited pid=%d uid=%u\n", p->pid, uid);
		yz_fd_handoff_release(p);
	}
}

int yz_lifecycle_enable(void)
{
	int ret;

	if (yz_lifecycle_fork_registered && yz_lifecycle_free_registered)
		return 0;

	ret = register_trace_sched_process_fork(yz_lifecycle_on_fork, NULL);

	if (ret) {
		pr_err(
		    "yukizygisk: fork tracepoint registration failed err=%d\n",
		    ret);
		return ret;
	}
	yz_lifecycle_fork_registered = true;

	ret = register_trace_sched_process_free(yz_lifecycle_on_free, NULL);
	if (ret) {
		pr_err(
		    "yukizygisk: free tracepoint registration failed err=%d\n",
		    ret);
		unregister_trace_sched_process_fork(yz_lifecycle_on_fork, NULL);
		tracepoint_synchronize_unregister();
		yz_lifecycle_fork_registered = false;
		return ret;
	}
	yz_lifecycle_free_registered = true;

	pr_info("yukizygisk: lifecycle tracking enabled\n");
	return 0;
}

void yz_lifecycle_disable(void)
{
	bool unregistered = false;

	if (yz_lifecycle_free_registered) {
		unregister_trace_sched_process_free(yz_lifecycle_on_free, NULL);
		yz_lifecycle_free_registered = false;
		unregistered = true;
	}
	if (yz_lifecycle_fork_registered) {
		unregister_trace_sched_process_fork(yz_lifecycle_on_fork, NULL);
		yz_lifecycle_fork_registered = false;
		unregistered = true;
	}
	if (unregistered)
		tracepoint_synchronize_unregister();
	yz_lifecycle_reset();
	yz_fd_handoff_cancel_all();
}

#else /* !CONFIG_TRACEPOINTS */

int yz_lifecycle_enable(void)
{
	pr_warn("yukizygisk: lifecycle tracking requires CONFIG_TRACEPOINTS\n");
	return -EOPNOTSUPP;
}

void yz_lifecycle_disable(void)
{
	yz_lifecycle_reset();
	yz_fd_handoff_cancel_all();
}

#endif /* CONFIG_TRACEPOINTS */

/* A successful UID transition identifies a tracked app child. */
void ksu_yukizygisk_on_setresuid(uid_t old_uid, uid_t new_uid)
{
	unsigned long flags;
	pid_t pid = current->pid;
	bool specialized = false;
	int i;

	(void)old_uid;
	if (!READ_ONCE(yukizygisk_enabled))
		return;

	if (new_uid < 10000) /* app uids only */
		return;

	/* Isolated app IDs 90000-99999 must not receive module FDs. */
	if (new_uid % 100000 >= 90000)
		return;

	spin_lock_irqsave(&yz_lifecycle_lock, flags);
	i = yz_lifecycle_slot_of(pid);
	if (i >= 0 && yz_lifecycle_children[i].state == YZ_LIFECYCLE_FORKED &&
	    yz_lifecycle_children[i].start_boottime ==
		current->start_boottime) {
		yz_lifecycle_children[i].uid = new_uid;
		yz_lifecycle_children[i].state = YZ_LIFECYCLE_SPECIALIZED;
		specialized = true;
	}
	spin_unlock_irqrestore(&yz_lifecycle_lock, flags);

	if (specialized) {
		pr_info("yukizygisk: app specialized pid=%d uid=%u appid=%u\n",
			pid, new_uid, new_uid % 100000);
		yz_emit_specialize(pid, new_uid % 100000);
	}
}
