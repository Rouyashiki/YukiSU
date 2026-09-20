#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "api.h"
#include "internal.h"
#include "uapi/yukizygisk.h"

struct yz_runtime_slot {
	struct yz_runtime_record record;
	u64 start_boottime;
};

static DEFINE_MUTEX(yz_runtime_lock);
static struct yz_runtime_slot yz_runtime_records[YZ_RUNTIME_RECORD_MAX];
static u32 yz_runtime_generation;

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
	task = get_pid_task(find_vpid((pid_t)pid), PIDTYPE_PID);
	rcu_read_unlock();
	if (!task)
		return ret;
	if (!READ_ONCE(task->exit_state)) {
		*start_boottime = READ_ONCE(task->start_boottime);
		ret = 0;
	}
	put_task_struct(task);
	return ret;
}

static bool yz_runtime_task_alive(u32 pid, u64 start_boottime)
{
	u64 current_start;

	return !yz_runtime_get_task_start(pid, &current_start) &&
	       current_start == start_boottime;
}

static void yz_runtime_refresh_exited_locked(void)
{
	u32 i;

	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *slot = &yz_runtime_records[i];

		if (!slot->record.pid ||
		    slot->record.state == YZ_RUNTIME_STATE_EXITED)
			continue;
		if (!yz_runtime_task_alive(slot->record.pid,
					   slot->start_boottime)) {
			slot->record.state = YZ_RUNTIME_STATE_EXITED;
			yz_runtime_advance_locked();
		}
	}
}

static struct yz_runtime_slot *
yz_runtime_alloc_slot_locked(const struct yz_runtime_slot *avoid)
{
	struct yz_runtime_slot *oldest_exited = NULL;
	struct yz_runtime_slot *oldest_module = NULL;
	u32 i;

	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *slot = &yz_runtime_records[i];

		if (slot == avoid)
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
	u32 i;

	mutex_lock(&yz_runtime_lock);
	yz_runtime_refresh_exited_locked();
	if (kind == YZ_RUNTIME_KIND_ZYGOTE) {
		for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
			struct yz_runtime_slot *candidate =
			    &yz_runtime_records[i];

			if (candidate->record.pid &&
			    candidate->record.kind == kind &&
			    candidate->record.abi == abi &&
			    !candidate->record.module_id[0] &&
			    !strcmp(candidate->record.target, target)) {
				slot = candidate;
				break;
			}
		}
	} else {
		for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
			struct yz_runtime_slot *candidate =
			    &yz_runtime_records[i];

			if (candidate->record.pid == pid &&
			    candidate->record.kind == kind &&
			    !candidate->record.module_id[0]) {
				slot = candidate;
				break;
			}
		}
	}

	if (slot && kind == YZ_RUNTIME_KIND_ZYGOTE) {
		restarts = slot->record.restarts + 1;
	} else if (kind == YZ_RUNTIME_KIND_NATIVE) {
		for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
			struct yz_runtime_record *record =
			    &yz_runtime_records[i].record;

			if (record->pid != pid || record->kind != kind ||
			    !record->module_id[0] ||
			    record->state == YZ_RUNTIME_STATE_EXITED)
				continue;
			record->state = YZ_RUNTIME_STATE_EXITED;
			yz_runtime_advance_locked();
		}
		if (!slot)
			slot = yz_runtime_alloc_slot_locked(NULL);
	} else {
		slot = yz_runtime_alloc_slot_locked(NULL);
	}
	if (!slot) {
		mutex_unlock(&yz_runtime_lock);
		return 0;
	}

	memset(slot, 0, sizeof(*slot));
	slot->record.pid = pid;
	slot->record.generation = yz_runtime_advance_locked();
	slot->record.restarts = restarts;
	slot->record.kind = kind;
	slot->record.state = YZ_RUNTIME_STATE_DETECTED;
	slot->record.abi = abi;
	slot->record.target_type = target_type;
	slot->record.flags = flags;
	yz_copy_name(slot->record.process, sizeof(slot->record.process),
		     process);
	yz_copy_name(slot->record.target, sizeof(slot->record.target), target);
	slot->start_boottime = start_boottime;
	i = slot->record.generation;
	mutex_unlock(&yz_runtime_lock);
	return i;
}

void yz_runtime_set_state(u32 pid, u32 generation, u8 state)
{
	u32 i;

	if (!generation)
		return;
	mutex_lock(&yz_runtime_lock);
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_record *record =
		    &yz_runtime_records[i].record;

		if (record->pid != pid || record->generation != generation ||
		    record->module_id[0])
			continue;
		if (record->state != state) {
			record->state = state;
			yz_runtime_advance_locked();
		}
		break;
	}
	mutex_unlock(&yz_runtime_lock);
}

int ksu_yukizygisk_get_runtime(struct yz_runtime_record *entries, u32 capacity,
			       struct yz_runtime_query_cmd *query)
{
	u32 count = 0;
	u32 i;

	if (!query || capacity > YZ_RUNTIME_RECORD_MAX ||
	    (capacity && !entries))
		return -EINVAL;

	mutex_lock(&yz_runtime_lock);
	yz_runtime_refresh_exited_locked();
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX && count < capacity; i++) {
		if (!yz_runtime_records[i].record.pid)
			continue;
		entries[count++] = yz_runtime_records[i].record;
	}
	query->count = count;
	query->generation = yz_runtime_generation;
	mutex_unlock(&yz_runtime_lock);

	yz_safemode_fill_runtime_query(query);
	return 0;
}

bool ksu_yukizygisk_is_native_runtime(pid_t pid, u64 start_boottime)
{
	bool found = false;
	u32 i;

	mutex_lock(&yz_runtime_lock);
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		const struct yz_runtime_slot *slot = &yz_runtime_records[i];
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
	mutex_unlock(&yz_runtime_lock);
	return found;
}

int ksu_yukizygisk_report_runtime(const struct yz_runtime_report_cmd *report)
{
	struct yz_runtime_slot *base = NULL;
	struct yz_runtime_slot *module = NULL;
	u64 start_boottime;
	u32 i;

	if (!report || !report->pid || !report->generation ||
	    (report->kind != YZ_RUNTIME_KIND_ZYGOTE &&
	     report->kind != YZ_RUNTIME_KIND_NATIVE) ||
	    (report->kind == YZ_RUNTIME_KIND_NATIVE && !report->module_id[0]))
		return -EINVAL;
	if (yz_runtime_get_task_start(report->pid, &start_boottime))
		return -ESRCH;

	mutex_lock(&yz_runtime_lock);
	yz_runtime_refresh_exited_locked();
	for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
		struct yz_runtime_slot *slot = &yz_runtime_records[i];

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
		mutex_unlock(&yz_runtime_lock);
		return -ESRCH;
	}
	if (!yz_runtime_task_alive(base->record.pid, base->start_boottime)) {
		base->record.state = YZ_RUNTIME_STATE_EXITED;
		yz_runtime_advance_locked();
		mutex_unlock(&yz_runtime_lock);
		return -ESRCH;
	}
	if (base->record.state != YZ_RUNTIME_STATE_REDIRECTED &&
	    base->record.state != YZ_RUNTIME_STATE_INJECTED) {
		mutex_unlock(&yz_runtime_lock);
		return -EAGAIN;
	}

	if (base->record.state != YZ_RUNTIME_STATE_INJECTED) {
		base->record.state = YZ_RUNTIME_STATE_INJECTED;
		yz_runtime_advance_locked();
	}
	if (report->kind == YZ_RUNTIME_KIND_NATIVE) {
		for (i = 0; i < YZ_RUNTIME_RECORD_MAX; i++) {
			struct yz_runtime_slot *slot = &yz_runtime_records[i];

			if (slot->record.pid == report->pid &&
			    slot->record.kind == YZ_RUNTIME_KIND_NATIVE &&
			    !strcmp(slot->record.module_id,
				    report->module_id)) {
				module = slot;
				break;
			}
		}
		if (!module)
			module = yz_runtime_alloc_slot_locked(base);
		if (!module) {
			mutex_unlock(&yz_runtime_lock);
			return -ENOSPC;
		}
		if (module->record.generation != base->record.generation ||
		    module->record.state != YZ_RUNTIME_STATE_INJECTED ||
		    module->start_boottime != base->start_boottime) {
			struct yz_runtime_record record = base->record;

			yz_copy_name(record.module_id, sizeof(record.module_id),
				     report->module_id);
			record.state = YZ_RUNTIME_STATE_INJECTED;
			memset(module, 0, sizeof(*module));
			module->record = record;
			module->start_boottime = base->start_boottime;
			yz_runtime_advance_locked();
		}
	}
	mutex_unlock(&yz_runtime_lock);
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
