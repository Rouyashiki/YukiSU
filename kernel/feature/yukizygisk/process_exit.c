#include <linux/errno.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/workqueue.h>
#include <trace/events/sched.h>

#include "internal.h"

static DEFINE_SPINLOCK(yz_exit_lock);
static bool yz_exit_enabled;
static struct tracepoint *yz_exit_tracepoint;

static void yz_exit_work_fn(struct work_struct *work)
{
	struct yz_target_exit_event event;

	(void)work;
	yz_exit_history_wake_readers();
	yz_load_policy_drain_exits();
	while (yz_runtime_take_exit(&event)) {
		struct yz_zygote_exit_event crash = {0};

		yz_emit_target_exit(&event);
		if (event.kind != YZ_RUNTIME_KIND_ZYGOTE ||
		    !(event.event.appid & 0x7f) ||
		    (event.event.appid & 0x7f) == SIGTERM)
			continue;
		crash.event = event.event;
		crash.event.type = YZ_EV_ZYGOTE_EXIT;
		crash.generation = event.generation;
		crash.start_boottime = event.start_boottime;
		crash.observed_boottime = event.observed_boottime;
		crash.abi = event.abi;
		yz_emit_zygote_exit(&crash);
	}
}

static DECLARE_WORK(yz_exit_work, yz_exit_work_fn);

bool yz_process_exit_active(void)
{
	return READ_ONCE(yz_exit_enabled);
}

void yz_process_exit_schedule(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_exit_lock, flags);
	if (yz_exit_enabled)
		schedule_work(&yz_exit_work);
	spin_unlock_irqrestore(&yz_exit_lock, flags);
}

#ifdef CONFIG_TRACEPOINTS
static void yz_exit_on_exit(void *data, struct task_struct *task
#ifdef YZ_EXIT_HAS_GROUP_DEAD
			    ,
			    bool group_dead
#endif
)
{
	bool pending, policy_pending;

	(void)data;
#ifdef YZ_EXIT_HAS_GROUP_DEAD
	if (!group_dead)
		return;
#else
	/* signal->live is decremented before this tracepoint on older kernels.
	 */
	if (atomic_read(&task->signal->live))
		return;
#endif
	pending = yz_runtime_on_exit(task);
	yz_lifecycle_on_exit(task);
	policy_pending = yz_load_policy_on_exit(task);
	if (pending || policy_pending)
		yz_process_exit_schedule();
}

static void yz_exit_find_tracepoint(struct tracepoint *tp, void *data)
{
	(void)data;
	if (!strcmp(tp->name, "sched_process_exit"))
		yz_exit_tracepoint = tp;
}
#endif

int yz_process_exit_enable(void)
{
#ifdef CONFIG_TRACEPOINTS
	unsigned long flags;
	int ret;

	if (READ_ONCE(yz_exit_enabled))
		return 0;
	for_each_kernel_tracepoint(yz_exit_find_tracepoint, NULL);
	if (!yz_exit_tracepoint)
		return -ENOENT;
	check_trace_callback_type_sched_process_exit(yz_exit_on_exit);
	ret = tracepoint_probe_register(yz_exit_tracepoint,
					(void *)yz_exit_on_exit, NULL);
	if (!ret) {
		yz_runtime_reconcile();
		yz_load_policy_reap();
		spin_lock_irqsave(&yz_exit_lock, flags);
		yz_exit_enabled = true;
		schedule_work(&yz_exit_work);
		spin_unlock_irqrestore(&yz_exit_lock, flags);
		yz_exit_history_set_active(true);
	}
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

void yz_process_exit_disable(void)
{
	unsigned long flags;
	bool enabled;

	spin_lock_irqsave(&yz_exit_lock, flags);
	enabled = yz_exit_enabled;
	yz_exit_enabled = false;
	spin_unlock_irqrestore(&yz_exit_lock, flags);
#ifdef CONFIG_TRACEPOINTS
	if (enabled) {
		tracepoint_probe_unregister(yz_exit_tracepoint,
					    (void *)yz_exit_on_exit, NULL);
		tracepoint_synchronize_unregister();
	}
#else
	(void)enabled;
#endif
	yz_exit_history_set_active(false);
	flush_work(&yz_exit_work);
	yz_exit_work_fn(&yz_exit_work);
	yz_exit_history_flush();
}
