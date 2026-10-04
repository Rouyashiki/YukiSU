#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/ktime.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "api.h"
#include "internal.h"
#include "uapi/yukizygisk.h"

struct yz_runtime_slot {
	struct hlist_node pid_node;
	struct yz_runtime_record record;
	u64 start_boottime;
	u64 exit_boottime;
	u32 exit_status;
	bool exit_pending;
	bool exit_observed;
};

static DEFINE_SPINLOCK(yz_runtime_lock);
static struct yz_runtime_slot yz_runtime_records[YZ_RUNTIME_RECORD_MAX];
static DEFINE_HASHTABLE(yz_runtime_pids, 8);
static u32 yz_runtime_generation;
static bool yz_runtime_reconcile_pending;

static bool yz_next_arg(unsigned long *p, unsigned long end, char *arg,
			size_t arg_len)
{
	char c = '\0';
	int i = 0;

	if (!p || !arg || !arg_len || *p >= end)
		return false;

	while (*p < end && i < (int)arg_len - 1) {
		if (get_user(c, (const char __user *)*p))
			return false;
		(*p)++;
		if (!c)
			break;
		arg[i++] = c;
	}
	arg[i] = '\0';

	/* If the argument was truncated, consume it so the next iteration
	 * starts at the next argv entry. */
	while (*p < end && c) {
		if (get_user(c, (const char __user *)*p))
			return false;
		(*p)++;
	}

	return true;
}

static u32 yz_runtime_advance_locked(void)
{
	if (++yz_runtime_generation == 0)
		yz_runtime_generation = 1;
	return yz_runtime_generation;
}

static int yz_runtime_get_task_start(u32 pid, u64 *start_boottime)
{
	struct task_struct *task;
	int ret = -ESRCH;

	rcu_read_lock();
	task = pid_task(find_pid_ns((pid_t)pid, &init_pid_ns), PIDTYPE_TGID);
	if (task && atomic_read(&task->signal->live) > 0) {
		*start_boottime = READ_ONCE(task->start_boottime);
		ret = 0;
	}
	rcu_read_unlock();
	return ret;
}

static bool yz_runtime_task_alive(u32 pid, u64 start_boottime)
{
	u64 current_start;

	return !yz_runtime_get_task_start(pid, &current_start) &&
	       current_start == start_boottime;
}

static void yz_runtime_mark_exited_locked(struct yz_runtime_record *record)
{
	if (record->state == YZ_RUNTIME_STATE_EXITED)
		return;
	record->flags &= ~YZ_RUNTIME_F_INJECTION_STATE_MASK;
	record->flags |= (u32)record->state
			 << YZ_RUNTIME_F_INJECTION_STATE_SHIFT;
	record->state = YZ_RUNTIME_STATE_EXITED;
}

static void yz_runtime_exit_slot_locked(struct yz_runtime_slot *slot,
					struct task_struct *task)
{
	if (slot->exit_observed)
		return;
	slot->exit_observed = true;
	slot->exit_status = READ_ONCE(task->signal->flags) & SIGNAL_GROUP_EXIT
				? READ_ONCE(task->signal->group_exit_code)
				: READ_ONCE(task->group_leader->exit_code);
	slot->exit_boottime = ktime_get_boottime_ns();
	slot->exit_pending = !slot->record.module_id[0];
	yz_runtime_mark_exited_locked(&slot->record);
	if (slot->exit_pending)
		yz_exit_history_append(&slot->record, slot->start_boottime,
				       slot->exit_boottime, slot->exit_status);
	yz_runtime_advance_locked();
}

static bool yz_runtime_refresh_pid_locked(u32 pid, bool *checked)
{
	struct yz_runtime_slot *slot;
	struct task_struct *task;
	bool observed = false;
	bool unsettled = false;

	rcu_read_lock();
	task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_TGID);
	hash_for_each_possible(yz_runtime_pids, slot, pid_node, pid)
	{
		if (slot->record.pid != pid)
			continue;
		if (checked)
			checked[slot - yz_runtime_records] = true;
		if (slot->record.state == YZ_RUNTIME_STATE_EXITED)
			continue;
		if (!task ||
		    READ_ONCE(task->start_boottime) != slot->start_boottime) {
			yz_runtime_mark_exited_locked(&slot->record);
			yz_runtime_advance_locked();
		} else if (!atomic_read(&task->signal->live)) {
			if (READ_ONCE(task->exit_state)) {
				yz_runtime_exit_slot_locked(slot, task);
				observed |= slot->exit_pending;
			} else {
				/* The task may have passed the tracepoint
				 * before monitoring started, but not published
				 * status. */
				unsettled = true;
			}
		}
	}
	rcu_read_unlock();
	if (observed)
		yz_process_exit_schedule();
	return unsettled;
}

static void yz_runtime_refresh_exited_locked(void)
{
	bool checked[YZ_RUNTIME_RECORD_MAX] = {false};
	bool unsettled = false;
	u32 i;

	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_record *record =
		    &yz_runtime_records[i].record;

		if (checked[i] || !record->pid ||
		    record->state == YZ_RUNTIME_STATE_EXITED)
			continue;
		unsettled |=
		    yz_runtime_refresh_pid_locked(record->pid, checked);
	}
	yz_runtime_reconcile_pending = unsettled;
}

void yz_runtime_reconcile(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_runtime_lock, flags);
	yz_runtime_refresh_exited_locked();
	spin_unlock_irqrestore(&yz_runtime_lock, flags);
}

bool yz_runtime_on_exit(struct task_struct *task)
{
	u64 start = READ_ONCE(task->group_leader->start_boottime);
	struct yz_runtime_slot *slot;
	unsigned long irqflags;
	bool pending = false;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	hash_for_each_possible(yz_runtime_pids, slot, pid_node, task->tgid)
	{
		if (slot->record.pid != (u32)task->tgid ||
		    slot->start_boottime != start || slot->exit_observed)
			continue;
		yz_runtime_exit_slot_locked(slot, task);
		pending |= slot->exit_pending;
	}
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
	return pending;
}

bool yz_runtime_take_exit(struct yz_target_exit_event *event)
{
	unsigned long irqflags;
	u32 i;
	bool found = false;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *slot = &yz_runtime_records[i];

		if (!slot->exit_pending)
			continue;
		memset(event, 0, sizeof(*event));
		event->event.type = YZ_EV_TARGET_EXIT;
		event->event.pid = slot->record.pid;
		event->event.appid = slot->exit_status;
		event->generation = slot->record.generation;
		event->start_boottime = slot->start_boottime;
		event->observed_boottime = slot->exit_boottime;
		event->abi = slot->record.abi;
		event->kind = slot->record.kind;
		slot->exit_pending = false;
		found = true;
		break;
	}
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
	return found;
}

static struct yz_runtime_slot *
yz_runtime_alloc_slot_locked(const struct yz_runtime_slot *avoid)
{
	struct yz_runtime_slot *oldest_exited = NULL;
	struct yz_runtime_slot *oldest_module = NULL;
	u32 i;

	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *slot = &yz_runtime_records[i];

		if (slot == avoid || slot->exit_pending)
			continue;
		if (!slot->record.pid)
			return slot;
		if (slot->record.state == YZ_RUNTIME_STATE_EXITED &&
		    (!oldest_exited || slot->record.generation <
					   oldest_exited->record.generation))
			oldest_exited = slot;
		if (slot->record.module_id[0] &&
		    (!oldest_module || slot->record.generation <
					   oldest_module->record.generation))
			oldest_module = slot;
	}
	return oldest_exited ? oldest_exited : oldest_module;
}

void yz_runtime_read_process(struct mm_struct *mm, char *process,
			     size_t process_len)
{
	unsigned long p, end;

	if (!process_len)
		return;
	process[0] = '\0';
	if (!mm)
		return;
	p = READ_ONCE(mm->arg_start);
	end = READ_ONCE(mm->arg_end);
	if (!p || end <= p)
		return;
	if (!yz_next_arg(&p, end, process, process_len))
		process[0] = '\0';
}

u32 yz_runtime_begin(u8 kind, u8 abi, u8 target_type, u32 flags,
		     const char *process, const char *target,
		     u64 start_boottime)
{
	struct yz_runtime_slot *slot = NULL;
	u32 restarts = 0;
	u32 pid = (u32)current->tgid;
	unsigned long irqflags;
	u32 i;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	if (!yz_process_exit_active() || yz_runtime_reconcile_pending)
		yz_runtime_refresh_exited_locked();
	else
		yz_runtime_refresh_pid_locked(pid, NULL);
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *candidate = &yz_runtime_records[i];

		if (!candidate->exit_pending && candidate->record.pid == pid &&
		    candidate->start_boottime == start_boottime &&
		    !candidate->record.module_id[0])
			slot = candidate;
	}
	if (kind == YZ_RUNTIME_KIND_ZYGOTE) {
		for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
			struct yz_runtime_slot *candidate =
			    &yz_runtime_records[i];

			if (!candidate->record.pid ||
			    candidate->record.kind != kind ||
			    candidate->record.abi != abi ||
			    candidate->record.module_id[0] ||
			    strcmp(candidate->record.target, target))
				continue;
			restarts =
			    max(restarts, candidate->record.restarts + 1);
			if (!slot && !candidate->exit_pending &&
			    candidate->record.state == YZ_RUNTIME_STATE_EXITED)
				slot = candidate;
		}
	}
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_record *record =
		    &yz_runtime_records[i].record;

		if (record->pid != pid || !record->module_id[0] ||
		    record->state == YZ_RUNTIME_STATE_EXITED)
			continue;
		yz_runtime_mark_exited_locked(record);
		yz_runtime_advance_locked();
	}
	if (!slot)
		slot = yz_runtime_alloc_slot_locked(NULL);
	if (!slot || (slot->record.pid &&
		      slot->record.state != YZ_RUNTIME_STATE_EXITED &&
		      slot->record.module_id[0])) {
		yz_runtime_refresh_exited_locked();
		slot = yz_runtime_alloc_slot_locked(NULL);
	}
	if (!slot) {
		spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
		return 0;
	}

	hash_del(&slot->pid_node);
	memset(slot, 0, sizeof(*slot));
	slot->record.pid = pid;
	slot->record.generation = yz_runtime_advance_locked();
	slot->record.restarts = restarts;
	slot->record.kind = kind;
	slot->record.state = YZ_RUNTIME_STATE_DETECTED;
	slot->record.abi = abi;
	slot->record.target_type = target_type;
	slot->record.flags = flags & ~YZ_RUNTIME_F_INJECTION_STATE_MASK;
	yz_copy_name(slot->record.process, sizeof(slot->record.process),
		     process);
	yz_copy_name(slot->record.target, sizeof(slot->record.target), target);
	slot->start_boottime = start_boottime;
	hash_add(yz_runtime_pids, &slot->pid_node, pid);
	i = slot->record.generation;
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
	return i;
}

void yz_runtime_set_state(u32 pid, u32 generation, u8 state)
{
	struct yz_runtime_slot *slot;
	unsigned long irqflags;

	if (!generation)
		return;
	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	hash_for_each_possible(yz_runtime_pids, slot, pid_node, pid)
	{
		struct yz_runtime_record *record = &slot->record;

		if (record->pid != pid || record->generation != generation ||
		    record->module_id[0] ||
		    record->state == YZ_RUNTIME_STATE_EXITED)
			continue;
		if (record->state != state) {
			if (state == YZ_RUNTIME_STATE_EXITED)
				yz_runtime_mark_exited_locked(record);
			else
				record->state = state;
			yz_runtime_advance_locked();
		}
		break;
	}
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
}

int ksu_yukizygisk_get_runtime(struct yz_runtime_record *entries, u32 capacity,
			       struct yz_runtime_query_cmd *query)
{
	unsigned long irqflags;
	u32 count = 0;
	u32 i;

	if (!query || capacity > YZ_RUNTIME_RECORD_MAX ||
	    (capacity && !entries))
		return -EINVAL;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	if (!yz_process_exit_active() || yz_runtime_reconcile_pending)
		yz_runtime_refresh_exited_locked();
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX && count < capacity; i++) {
		if (!yz_runtime_records[i].record.pid)
			continue;
		entries[count++] = yz_runtime_records[i].record;
	}
	query->count = count;
	query->generation = yz_runtime_generation;
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);

	yz_safemode_fill_runtime_query(query);
	query->capabilities |=
	    YZ_RUNTIME_CAP_EXIT_HISTORY | YZ_RUNTIME_CAP_HEALTH;
	return 0;
}

bool ksu_yukizygisk_is_native_runtime(pid_t pid, u64 start_boottime)
{
	struct yz_runtime_slot *slot;
	unsigned long irqflags;
	bool found = false;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	hash_for_each_possible(yz_runtime_pids, slot, pid_node, pid)
	{
		const struct yz_runtime_record *record = &slot->record;

		if (record->pid == (u32)pid &&
		    slot->start_boottime == start_boottime &&
		    record->kind == YZ_RUNTIME_KIND_NATIVE &&
		    !record->module_id[0] &&
		    (record->state == YZ_RUNTIME_STATE_REDIRECTED ||
		     record->state == YZ_RUNTIME_STATE_INJECTED)) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
	return found;
}

int ksu_yukizygisk_report_runtime(const struct yz_runtime_report_cmd *report)
{
	unsigned long irqflags;
	struct yz_runtime_slot *base = NULL;
	struct yz_runtime_slot *module = NULL;
	struct yz_runtime_slot *slot;
	u64 start_boottime;
	u8 state;

	if (!report || !report->pid || !report->generation ||
	    (report->kind != YZ_RUNTIME_KIND_ZYGOTE &&
	     report->kind != YZ_RUNTIME_KIND_NATIVE) ||
	    (report->kind == YZ_RUNTIME_KIND_NATIVE && !report->module_id[0]) ||
	    (report->module_state &&
	     report->module_state != YZ_RUNTIME_STATE_INJECTED &&
	     report->module_state != YZ_RUNTIME_STATE_FAILED &&
	     report->module_state != YZ_RUNTIME_STATE_SAFEMODE) ||
	    (!report->module_id[0] && report->module_state &&
	     report->module_state != YZ_RUNTIME_STATE_INJECTED))
		return -EINVAL;
	state = report->module_state ? report->module_state
				     : YZ_RUNTIME_STATE_INJECTED;
	if (yz_runtime_get_task_start(report->pid, &start_boottime))
		return -ESRCH;

	spin_lock_irqsave(&yz_runtime_lock, irqflags);
	hash_for_each_possible(yz_runtime_pids, slot, pid_node, report->pid)
	{
		if (slot->record.pid != report->pid ||
		    slot->record.generation != report->generation ||
		    slot->record.kind != report->kind ||
		    slot->start_boottime != start_boottime ||
		    slot->record.module_id[0] ||
		    slot->record.state == YZ_RUNTIME_STATE_EXITED)
			continue;
		if (!base || slot->record.generation > base->record.generation)
			base = slot;
	}
	if (!base) {
		spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
		return -ESRCH;
	}
	if (!yz_runtime_task_alive(base->record.pid, base->start_boottime)) {
		yz_runtime_refresh_pid_locked(base->record.pid, NULL);
		spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
		return -ESRCH;
	}
	if (base->record.state != YZ_RUNTIME_STATE_REDIRECTED &&
	    base->record.state != YZ_RUNTIME_STATE_INJECTED) {
		spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
		return -EAGAIN;
	}

	if ((!report->module_id[0] || report->kind == YZ_RUNTIME_KIND_NATIVE) &&
	    state == YZ_RUNTIME_STATE_INJECTED &&
	    base->record.state != YZ_RUNTIME_STATE_INJECTED) {
		base->record.state = YZ_RUNTIME_STATE_INJECTED;
		yz_runtime_advance_locked();
	}
	if (report->module_id[0]) {
		hash_for_each_possible(yz_runtime_pids, slot, pid_node,
				       report->pid)
		{
			if (slot->record.pid == report->pid &&
			    slot->record.kind == report->kind &&
			    slot->record.generation == report->generation &&
			    slot->start_boottime == start_boottime &&
			    !strcmp(slot->record.module_id,
				    report->module_id)) {
				module = slot;
				break;
			}
		}
		if (!module) {
			module = yz_runtime_alloc_slot_locked(base);
			if (!module ||
			    (module->record.pid &&
			     module->record.state != YZ_RUNTIME_STATE_EXITED)) {
				yz_runtime_refresh_exited_locked();
				if (base->record.state ==
					YZ_RUNTIME_STATE_EXITED ||
				    !yz_runtime_task_alive(
					base->record.pid,
					base->start_boottime)) {
					yz_runtime_refresh_pid_locked(
					    base->record.pid, NULL);
					spin_unlock_irqrestore(&yz_runtime_lock,
							       irqflags);
					return -ESRCH;
				}
				module = yz_runtime_alloc_slot_locked(base);
			}
		}
		if (!module) {
			spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
			return -ENOSPC;
		}
		/* Preserve failed child loads for this Zygote generation. */
		if (report->kind == YZ_RUNTIME_KIND_ZYGOTE &&
		    module->record.generation == base->record.generation &&
		    module->start_boottime == base->start_boottime &&
		    !strcmp(module->record.module_id, report->module_id) &&
		    (module->record.state == YZ_RUNTIME_STATE_FAILED ||
		     module->record.state == YZ_RUNTIME_STATE_SAFEMODE))
			state = module->record.state;
		if (module->record.generation != base->record.generation ||
		    module->record.state != state ||
		    strcmp(module->record.module_id, report->module_id) ||
		    module->start_boottime != base->start_boottime) {
			struct yz_runtime_record record = base->record;

			yz_copy_name(record.module_id, sizeof(record.module_id),
				     report->module_id);
			record.state = state;
			record.flags &= ~YZ_RUNTIME_F_INJECTION_STATE_MASK;
			hash_del(&module->pid_node);
			memset(module, 0, sizeof(*module));
			module->record = record;
			module->start_boottime = base->start_boottime;
			hash_add(yz_runtime_pids, &module->pid_node,
				 report->pid);
			yz_runtime_advance_locked();
		}
	}
	spin_unlock_irqrestore(&yz_runtime_lock, irqflags);
	return 0;
}

bool yz_parse_zygote_args(struct mm_struct *mm, char *socket_name,
			  size_t socket_name_len)
{
	unsigned long p, end;
	char arg[96];
	bool found = false;
	int argc = 0;

	if (!mm)
		return false;
	if (socket_name_len)
		socket_name[0] = '\0';
	p = READ_ONCE(mm->arg_start);
	end = READ_ONCE(mm->arg_end);
	if (!p || end <= p)
		return false;

	while (p < end && argc++ < 64) {
		static const char socket_prefix[] = "--socket-name=";

		if (!yz_next_arg(&p, end, arg, sizeof(arg)))
			return false;
		if (!strcmp(arg, "-Xzygote"))
			found = true;
		else if (!strncmp(arg, socket_prefix,
				  sizeof(socket_prefix) - 1))
			yz_copy_name(socket_name, socket_name_len,
				     arg + sizeof(socket_prefix) - 1);
	}

	if (found && socket_name_len && socket_name[0] == '\0')
		yz_copy_name(socket_name, socket_name_len, "zygote");
	return found;
}
