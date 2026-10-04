#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "api.h"
#include "internal.h"
#include "uapi/yukizygisk.h"
#include "klog.h" // IWYU pragma: keep

struct yz_fd_handoff {
	struct list_head node;
	struct callback_head twork;
	struct completion done;
	refcount_t refs;
	struct task_struct *task;
	struct mm_struct *mm;
	pid_t tgid;
	u64 start_boottime;
	bool cancelled;
	bool running;
	unsigned int n;
	struct file *files[YZ_MAX_MODULE_FDS];
};

#define YZ_MAX_PENDING 64
static LIST_HEAD(yz_handoffs);
static DEFINE_SPINLOCK(yz_handoff_lock);
static DEFINE_MUTEX(yz_handoff_mutex);
static DECLARE_WAIT_QUEUE_HEAD(yz_handoff_wait);
static atomic_t yz_handoff_active = ATOMIC_INIT(0);
static unsigned int yz_handoff_count;
static bool yz_handoff_stopping = true;

static void yz_handoff_put(struct yz_fd_handoff *p)
{
	unsigned int i;

	if (!refcount_dec_and_test(&p->refs))
		return;
	for (i = 0; i < p->n; i++)
		fput(p->files[i]);
	if (p->mm)
		mmdrop(p->mm);
	if (p->task)
		put_task_struct(p->task);
	kfree(p);
}

static void yz_handoff_finish(struct yz_fd_handoff *p)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_handoff_lock, flags);
	list_del(&p->node);
	yz_handoff_count--;
	spin_unlock_irqrestore(&yz_handoff_lock, flags);
	complete_all(&p->done);
	yz_handoff_put(p);
	if (atomic_dec_and_test(&yz_handoff_active))
		wake_up_all(&yz_handoff_wait);
}

/* The queued node stays owned by task_work until delivery or exact
 * cancellation. */
static void yz_handoff_deliver(struct callback_head *head)
{
	struct yz_fd_handoff *p =
	    container_of(head, struct yz_fd_handoff, twork);
	struct file *files[YZ_MAX_MODULE_FDS];
	unsigned long flags;
	unsigned int n, i;
	bool cancelled;
	int fd;

	spin_lock_irqsave(&yz_handoff_lock, flags);
	p->running = true;
	cancelled = p->cancelled || yz_handoff_stopping;
	n = p->n;
	for (i = 0; i < n; i++)
		files[i] = p->files[i];
	p->n = 0;
	spin_unlock_irqrestore(&yz_handoff_lock, flags);

	cancelled |= current != p->task || current->mm != p->mm ||
		     (current->flags & PF_EXITING);
	for (i = 0; i < n; i++) {
		if (cancelled) {
			fput(files[i]);
			continue;
		}
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			fput(files[i]);
			continue;
		}
		fd_install(fd, files[i]);
	}

	yz_handoff_finish(p);
}

static bool yz_handoff_matches(struct callback_head *head, void *data)
{
	return head == data;
}

/* yz_handoff_mutex serializes queue publication and cancellation. */
static void yz_handoff_reap_locked(void)
{
	struct yz_fd_handoff *p, *pending;
	unsigned long flags;

	for (;;) {
		pending = NULL;
		spin_lock_irqsave(&yz_handoff_lock, flags);
		list_for_each_entry (p, &yz_handoffs, node) {
			if (p->cancelled) {
				refcount_inc(&p->refs);
				pending = p;
				break;
			}
		}
		spin_unlock_irqrestore(&yz_handoff_lock, flags);
		if (!pending)
			return;

		if (task_work_cancel_match(pending->task, yz_handoff_matches,
					   &pending->twork))
			yz_handoff_finish(pending);
		else
			wait_for_completion(&pending->done);
		yz_handoff_put(pending);
	}
}

static void yz_handoff_reap_work_fn(struct work_struct *work)
{
	(void)work;
	mutex_lock(&yz_handoff_mutex);
	yz_handoff_reap_locked();
	mutex_unlock(&yz_handoff_mutex);
}

static DECLARE_WORK(yz_handoff_reap_work, yz_handoff_reap_work_fn);

int ksu_yukizygisk_handoff_module_fds(void __user *arg)
{
	struct yz_handoff_cmd cmd;
	struct yz_fd_handoff *p, *existing;
	struct file *old[YZ_MAX_MODULE_FDS];
	struct mm_struct *mm;
	unsigned long flags;
	unsigned int i, n_old = 0;
	bool reap = false;
	int ret = 0;

	if (!IS_ENABLED(CONFIG_TASKS_RCU))
		return -EOPNOTSUPP;
	if (copy_from_user(&cmd, arg, sizeof(cmd)))
		return -EFAULT;
	if (!cmd.pid || cmd.n_fds > YZ_MAX_MODULE_FDS)
		return -EINVAL;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	refcount_set(&p->refs, 1);
	init_completion(&p->done);
	init_task_work(&p->twork, yz_handoff_deliver);
	for (i = 0; i < cmd.n_fds; i++) {
		p->files[i] = fget(cmd.fds[i]);
		if (!p->files[i]) {
			ret = -EBADF;
			goto out;
		}
		p->n++;
	}

	rcu_read_lock();
	p->task = find_task_by_vpid(cmd.pid);
	if (p->task) {
		get_task_struct(p->task);
		p->tgid = p->task->tgid;
		p->start_boottime =
		    READ_ONCE(p->task->group_leader->start_boottime);
	}
	rcu_read_unlock();
	if (!p->task) {
		ret = -ESRCH;
		goto out;
	}
	mm = get_task_mm(p->task);
	if (!mm) {
		ret = -ESRCH;
		goto out;
	}
	mmgrab(mm);
	mmput(mm);
	p->mm = mm;

	mutex_lock(&yz_handoff_mutex);
	spin_lock_irqsave(&yz_handoff_lock, flags);
	if (yz_handoff_stopping || !READ_ONCE(yukizygisk_enabled)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if ((READ_ONCE(p->task->flags) & PF_EXITING) ||
	    READ_ONCE(p->task->mm) != p->mm) {
		ret = -ESRCH;
		goto unlock;
	}
	list_for_each_entry (existing, &yz_handoffs, node) {
		if (existing->task != p->task)
			continue;
		if (existing->mm != p->mm) {
			existing->cancelled = true;
			reap = true;
			continue;
		}
		if (existing->cancelled)
			continue;
		if (existing->running) {
			ret = -EBUSY;
			goto unlock;
		}
		for (i = 0; i < existing->n; i++)
			old[n_old++] = existing->files[i];
		existing->n = p->n;
		for (i = 0; i < p->n; i++)
			existing->files[i] = p->files[i];
		p->n = 0;
		goto unlock;
	}
	if (yz_handoff_count >= YZ_MAX_PENDING) {
		ret = -ENOSPC;
		goto unlock;
	}
	list_add_tail(&p->node, &yz_handoffs);
	yz_handoff_count++;
	atomic_inc(&yz_handoff_active);
	ret = task_work_add(p->task, &p->twork, TWA_RESUME);
	if (ret) {
		list_del(&p->node);
		yz_handoff_count--;
		atomic_dec(&yz_handoff_active);
	} else {
		p = NULL;
	}
unlock:
	spin_unlock_irqrestore(&yz_handoff_lock, flags);
	if (reap)
		schedule_work(&yz_handoff_reap_work);
	mutex_unlock(&yz_handoff_mutex);
	for (i = 0; i < n_old; i++)
		fput(old[i]);
out:
	if (p)
		yz_handoff_put(p);
	return ret;
}

void yz_fd_handoff_release(struct task_struct *task)
{
	struct yz_fd_handoff *p;
	unsigned long flags;
	bool pending = false;

	if (!atomic_read(&yz_handoff_active))
		return;
	spin_lock_irqsave(&yz_handoff_lock, flags);
	list_for_each_entry (p, &yz_handoffs, node) {
		if (p->tgid == task->tgid &&
		    p->start_boottime ==
			READ_ONCE(task->group_leader->start_boottime)) {
			p->cancelled = true;
			pending = true;
		}
	}
	if (pending && !yz_handoff_stopping)
		schedule_work(&yz_handoff_reap_work);
	spin_unlock_irqrestore(&yz_handoff_lock, flags);
}

void yz_fd_handoff_cancel_all(void)
{
	struct yz_fd_handoff *p;
	unsigned long flags;

	mutex_lock(&yz_handoff_mutex);
	spin_lock_irqsave(&yz_handoff_lock, flags);
	list_for_each_entry (p, &yz_handoffs, node)
		p->cancelled = true;
	spin_unlock_irqrestore(&yz_handoff_lock, flags);
	yz_handoff_reap_locked();
	mutex_unlock(&yz_handoff_mutex);
	flush_work(&yz_handoff_reap_work);
	wait_event(yz_handoff_wait, !atomic_read(&yz_handoff_active));
}

void yz_fd_handoff_init(void)
{
	WRITE_ONCE(yz_handoff_stopping, false);
}

void yz_fd_handoff_exit(void)
{
	unsigned long flags;

	mutex_lock(&yz_handoff_mutex);
	spin_lock_irqsave(&yz_handoff_lock, flags);
	yz_handoff_stopping = true;
	spin_unlock_irqrestore(&yz_handoff_lock, flags);
	mutex_unlock(&yz_handoff_mutex);
	yz_fd_handoff_cancel_all();
	/* Completion precedes the final callback epilogue; wait for its return.
	 */
	synchronize_rcu_tasks();
}
