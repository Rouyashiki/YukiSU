#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "api.h"
#include "internal.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

#define YZ_NATIVE_POLICY_TIMEOUT (10 * HZ)
#define YZ_MODULE_POLICY_TIMEOUT (10 * HZ)
#define YZ_POLICY_EXIT_CAPACITY 64
#define YZ_POLICY_STATE_CAPACITY 256
#define YZ_POLICY_HOLDER_CAPACITY 512

enum yz_policy_kind {
	YZ_POLICY_NATIVE,
	YZ_POLICY_MODULE,
};

enum yz_policy_phase {
	YZ_POLICY_PREPARING,
	YZ_POLICY_ACTIVE,
	YZ_POLICY_RETIRED,
};

struct yz_policy_state {
	struct list_head retry;
	struct list_head retired;
	struct ksu_file_load_policy state;
	u64 retired_boottime;
	unsigned long retry_at;
	unsigned int retries;
	enum yz_policy_kind kind;
	enum yz_policy_phase phase;
	bool retry_waiting;
};

struct yz_policy_watch {
	struct hlist_node node;
	struct pid *owner;
};

struct yz_native_policy_pending {
	struct list_head list;
	struct hlist_node owner_node;
	struct yz_policy_watch watch;
	struct yz_policy_state policy;
	struct delayed_work timeout;
	bool pending;
};

struct yz_module_policy_group {
	struct list_head list;
	struct yz_policy_state policy;
	u32 users;
};

struct yz_module_policy_holder {
	struct list_head list;
	struct hlist_node owner_node;
	struct yz_module_policy_group *group;
	struct delayed_work timeout;
	struct yz_policy_watch watch;
	bool pending;
};

static DECLARE_RWSEM(yz_policy_admission);
static bool yz_policy_enabled;
static atomic_t yz_policy_states = ATOMIC_INIT(0);
static atomic_t yz_policy_holders = ATOMIC_INIT(0);
static DEFINE_MUTEX(yz_native_policy_lock);
static LIST_HEAD(yz_native_policy_pending);
static DEFINE_HASHTABLE(yz_native_policy_owners, 8);
static DEFINE_MUTEX(yz_module_policy_lock);
static LIST_HEAD(yz_module_policy_groups);
static LIST_HEAD(yz_module_policy_holders);
static DEFINE_HASHTABLE(yz_module_policy_owners, 8);
static DEFINE_SPINLOCK(yz_policy_watch_lock);
static DEFINE_HASHTABLE(yz_policy_watches, 8);
static struct pid *yz_policy_exit_owners[YZ_POLICY_EXIT_CAPACITY];
static unsigned int yz_policy_exit_count;
static bool yz_policy_exit_accepting;
static bool yz_policy_reconcile_required;
static struct yz_health_cleanup yz_cleanup_health;
static DEFINE_MUTEX(yz_policy_retry_lock);
static LIST_HEAD(yz_policy_retries);
static DEFINE_SPINLOCK(yz_policy_health_lock);
static struct yz_health_policy yz_policy_health;
static LIST_HEAD(yz_policy_retired);

static void yz_policy_retry_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(yz_policy_retry_work, yz_policy_retry_work_fn);

void yz_load_policy_fill_health(struct yz_health_policy *policy,
				struct yz_health_cleanup *cleanup)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_health_lock, flags);
	*policy = yz_policy_health;
	if (!list_empty(&yz_policy_retired)) {
		struct yz_policy_state *oldest = list_first_entry(
		    &yz_policy_retired, struct yz_policy_state, retired);

		policy->oldest_retired_boottime = oldest->retired_boottime;
	}
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	*cleanup = yz_cleanup_health;
	cleanup->queued_owners = yz_policy_exit_count;
	cleanup->reconcile_pending = yz_policy_reconcile_required;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

void yz_load_policy_note_exit_work(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.exit_worker_runs++;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

static void yz_policy_activate(struct yz_policy_state *policy)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.preparing--;
	if (policy->kind == YZ_POLICY_NATIVE)
		yz_policy_health.native_active++;
	else
		yz_policy_health.module_groups_active++;
	policy->phase = YZ_POLICY_ACTIVE;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
}

static void yz_policy_retire(struct yz_policy_state *policy)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_health_lock, flags);
	if (policy->phase != YZ_POLICY_RETIRED) {
		if (policy->phase == YZ_POLICY_PREPARING)
			yz_policy_health.preparing--;
		else if (policy->kind == YZ_POLICY_NATIVE)
			yz_policy_health.native_active--;
		else
			yz_policy_health.module_groups_active--;
		policy->phase = YZ_POLICY_RETIRED;
		policy->retired_boottime = ktime_get_boottime_ns();
		list_add_tail(&policy->retired, &yz_policy_retired);
		yz_policy_health.retired_current++;
	}
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
}

static bool yz_policy_has_additions(const struct ksu_file_load_policy *state)
{
	return state->added_av || state->tmpfs_added_av ||
	       state->process_added_av || state->dir_added_av;
}

static int yz_policy_reserve(void)
{
	unsigned long flags;

	if (atomic_inc_return(&yz_policy_states) > YZ_POLICY_STATE_CAPACITY) {
		atomic_dec(&yz_policy_states);
		return -ENOSPC;
	}
	if (!try_module_get(THIS_MODULE)) {
		atomic_dec(&yz_policy_states);
		return -ESHUTDOWN;
	}
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.states_current++;
	yz_policy_health.preparing++;
	if (yz_policy_health.states_peak < yz_policy_health.states_current)
		yz_policy_health.states_peak = yz_policy_health.states_current;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	return 0;
}

static void yz_policy_unreserve(void)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.states_current--;
	yz_policy_health.preparing--;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	atomic_dec(&yz_policy_states);
	module_put(THIS_MODULE);
}

static void yz_policy_free(struct yz_policy_state *policy)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_health_lock, flags);
	list_del_init(&policy->retired);
	yz_policy_health.retired_current--;
	yz_policy_health.states_current--;
	yz_policy_health.restore_inflight--;
	yz_policy_health.restore_successes++;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	if (policy->kind == YZ_POLICY_NATIVE) {
		struct yz_native_policy_pending *entry = container_of(
		    policy, struct yz_native_policy_pending, policy);

		put_pid(entry->watch.owner);
		kfree(entry);
	} else {
		kfree(container_of(policy, struct yz_module_policy_group,
				   policy));
	}
	atomic_dec(&yz_policy_states);
	module_put(THIS_MODULE);
}

static void yz_policy_retry_schedule_locked(void)
{
	struct yz_policy_state *policy;
	unsigned long next = 0;
	unsigned long now;
	bool found = false;

	list_for_each_entry (policy, &yz_policy_retries, retry) {
		if (!found || time_before(policy->retry_at, next)) {
			next = policy->retry_at;
			found = true;
		}
	}
	if (found) {
		now = jiffies;
		mod_delayed_work(system_wq, &yz_policy_retry_work,
				 time_after(next, now) ? next - now : 0);
	}
}

static void yz_policy_restore(struct yz_policy_state *policy)
{
	unsigned long delay;
	unsigned long flags;
	int ret;

	yz_policy_retire(policy);
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	if (policy->retry_waiting) {
		policy->retry_waiting = false;
		yz_policy_health.retry_waiting--;
	}
	yz_policy_health.restore_inflight++;
	yz_policy_health.restore_attempts++;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	ret = ksu_file_load_policy_restore(&policy->state);

	if (!ret) {
		yz_policy_free(policy);
		return;
	}
	delay = policy->retries == 0   ? msecs_to_jiffies(100)
		: policy->retries == 1 ? HZ
				       : 10 * HZ;
	if (policy->retries < 2)
		policy->retries++;
	policy->retry_at = jiffies + delay;
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.restore_inflight--;
	yz_policy_health.restore_failures++;
	yz_policy_health.retry_waiting++;
	policy->retry_waiting = true;
	yz_policy_health.last_restore_errno = ret;
	yz_policy_health.last_restore_kind = policy->kind == YZ_POLICY_NATIVE
						 ? YZ_HEALTH_POLICY_NATIVE
						 : YZ_HEALTH_POLICY_MODULE;
	yz_policy_health.last_restore_boottime = ktime_get_boottime_ns();
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	pr_err_ratelimited("yukizygisk: load policy restore pending kind=%u "
			   "source=%u target=%u err=%d\n",
			   policy->kind, policy->state.src_type,
			   policy->state.tgt_type, ret);
	mutex_lock(&yz_policy_retry_lock);
	list_add_tail(&policy->retry, &yz_policy_retries);
	yz_policy_retry_schedule_locked();
	mutex_unlock(&yz_policy_retry_lock);
}

static void yz_policy_retry_work_fn(struct work_struct *work)
{
	struct yz_policy_state *policy, *tmp;
	unsigned long flags;
	LIST_HEAD(todo);

	(void)work;
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.retry_worker_runs++;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	mutex_lock(&yz_policy_retry_lock);
	list_for_each_entry_safe (policy, tmp, &yz_policy_retries, retry) {
		if (time_after_eq(jiffies, policy->retry_at))
			list_move_tail(&policy->retry, &todo);
	}
	mutex_unlock(&yz_policy_retry_lock);
	list_for_each_entry_safe (policy, tmp, &todo, retry) {
		list_del_init(&policy->retry);
		yz_policy_restore(policy);
	}
	mutex_lock(&yz_policy_retry_lock);
	yz_policy_retry_schedule_locked();
	mutex_unlock(&yz_policy_retry_lock);
}

static void yz_policy_watch_add(struct yz_policy_watch *watch)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	hash_add(yz_policy_watches, &watch->node, pid_nr(watch->owner));
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

static void yz_policy_watch_del(struct yz_policy_watch *watch)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	hash_del(&watch->node);
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

static bool yz_policy_queue_owner(struct pid *owner)
{
	struct yz_policy_watch *watch;
	unsigned long flags;
	unsigned int i;
	bool found = false;

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	if (!yz_policy_exit_accepting)
		goto out;
	hash_for_each_possible(yz_policy_watches, watch, node, pid_nr(owner))
	{
		yz_cleanup_health.watch_index_visits++;
		if (watch->owner == owner) {
			found = true;
			break;
		}
	}
	if (!found)
		goto out;
	for (i = 0; i < yz_policy_exit_count; i++)
		if (yz_policy_exit_owners[i] == owner)
			goto out;
	if (yz_policy_exit_count == YZ_POLICY_EXIT_CAPACITY) {
		yz_policy_reconcile_required = true;
		yz_cleanup_health.queue_overflows++;
		goto out;
	}
	yz_policy_exit_owners[yz_policy_exit_count++] = get_pid(owner);
	yz_cleanup_health.queue_enqueued++;
	if (yz_cleanup_health.queue_peak < yz_policy_exit_count)
		yz_cleanup_health.queue_peak = yz_policy_exit_count;
out:
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	return found;
}

bool yz_load_policy_on_exit(struct task_struct *task)
{
	return yz_policy_queue_owner(task_tgid(task));
}

static bool yz_policy_owner_alive(struct pid *owner, bool publication)
{
	struct task_struct *task;
	unsigned long flags;
	bool alive;

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	if (publication)
		yz_cleanup_health.alive_checks_publish++;
	else
		yz_cleanup_health.alive_checks_scan++;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	rcu_read_lock();
	task = pid_task(owner, PIDTYPE_TGID);
	alive = task && atomic_read(&task->signal->live) > 0;
	rcu_read_unlock();
	return alive;
}

static struct pid *yz_policy_get_owner(pid_t tgid)
{
	struct task_struct *task;
	struct pid *owner = NULL;

	rcu_read_lock();
	task = pid_task(find_pid_ns(tgid, &init_pid_ns), PIDTYPE_TGID);
	if (task && atomic_read(&task->signal->live) > 0)
		owner = get_pid(task_tgid(task));
	rcu_read_unlock();
	return owner;
}

static void yz_native_policy_detach(struct yz_native_policy_pending *entry,
				    struct list_head *todo)
{
	entry->pending = false;
	yz_policy_retire(&entry->policy);
	hash_del(&entry->owner_node);
	yz_policy_watch_del(&entry->watch);
	list_move_tail(&entry->list, todo);
}

static void yz_native_policy_timeout(struct work_struct *work)
{
	struct yz_native_policy_pending *entry = container_of(
	    to_delayed_work(work), struct yz_native_policy_pending, timeout);
	LIST_HEAD(todo);
	bool release = false;

	mutex_lock(&yz_native_policy_lock);
	if (entry->pending) {
		yz_native_policy_detach(entry, &todo);
		list_del_init(&entry->list);
		release = true;
	}
	mutex_unlock(&yz_native_policy_lock);
	if (release)
		yz_policy_restore(&entry->policy);
}

static void yz_release_native_policies(struct list_head *entries)
{
	struct yz_native_policy_pending *entry, *tmp;

	list_for_each_entry_safe (entry, tmp, entries, list) {
		cancel_delayed_work_sync(&entry->timeout);
		list_del_init(&entry->list);
		yz_policy_restore(&entry->policy);
	}
}

struct ksu_file_load_policy *yz_native_policy_begin(void)
{
	struct yz_native_policy_pending *entry;
	int ret;

	down_read(&yz_policy_admission);
	if (!yz_policy_enabled) {
		ret = -ESHUTDOWN;
		goto fail;
	}
	ret = yz_policy_reserve();
	if (ret)
		goto fail;
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		yz_policy_unreserve();
		ret = -ENOMEM;
		goto fail;
	}
	entry->watch.owner = get_pid(task_tgid(current));
	entry->policy.kind = YZ_POLICY_NATIVE;
	INIT_LIST_HEAD(&entry->policy.retry);
	INIT_LIST_HEAD(&entry->list);
	INIT_DELAYED_WORK(&entry->timeout, yz_native_policy_timeout);
	return &entry->policy.state;
fail:
	up_read(&yz_policy_admission);
	return ERR_PTR(ret);
}

void yz_restore_native_policy_state(struct ksu_file_load_policy *state)
{
	struct yz_policy_state *policy =
	    container_of(state, struct yz_policy_state, state);

	yz_policy_restore(policy);
	up_read(&yz_policy_admission);
}

void yz_publish_native_policy_state(struct ksu_file_load_policy *state)
{
	struct yz_native_policy_pending *entry =
	    container_of(state, struct yz_native_policy_pending, policy.state);
	struct yz_native_policy_pending *cur;
	struct hlist_node *tmp;
	LIST_HEAD(old_entries);
	bool schedule = false;

	if (!yz_policy_has_additions(state)) {
		yz_restore_native_policy_state(state);
		return;
	}
	mutex_lock(&yz_native_policy_lock);
	hash_for_each_possible_safe(yz_native_policy_owners, cur, tmp,
				    owner_node, pid_nr(entry->watch.owner))
	{
		if (cur->watch.owner == entry->watch.owner)
			yz_native_policy_detach(cur, &old_entries);
	}
	entry->pending = true;
	yz_policy_activate(&entry->policy);
	list_add_tail(&entry->list, &yz_native_policy_pending);
	hash_add(yz_native_policy_owners, &entry->owner_node,
		 pid_nr(entry->watch.owner));
	yz_policy_watch_add(&entry->watch);
	schedule_delayed_work(&entry->timeout, YZ_NATIVE_POLICY_TIMEOUT);
	if (!yz_policy_owner_alive(entry->watch.owner, true))
		schedule = yz_policy_queue_owner(entry->watch.owner);
	mutex_unlock(&yz_native_policy_lock);
	yz_release_native_policies(&old_entries);
	up_read(&yz_policy_admission);
	if (schedule)
		yz_process_exit_schedule();
}

static struct yz_module_policy_group *
yz_find_module_policy_group(const struct ksu_file_load_policy *state)
{
	struct yz_module_policy_group *group;

	list_for_each_entry (group, &yz_module_policy_groups, list) {
		const struct ksu_file_load_policy *cur = &group->policy.state;

		if (cur->src_type == state->src_type &&
		    cur->tgt_type == state->tgt_type &&
		    cur->tmpfs_type == state->tmpfs_type &&
		    cur->process_type == state->process_type &&
		    cur->target_class == state->target_class &&
		    cur->process_class == state->process_class &&
		    cur->dir_class == state->dir_class &&
		    cur->added_av == state->added_av &&
		    cur->tmpfs_added_av == state->tmpfs_added_av &&
		    cur->dir_added_av == state->dir_added_av &&
		    cur->process_added_av == state->process_added_av)
			return group;
	}
	return NULL;
}

static struct yz_module_policy_holder *
yz_find_module_policy_holder(struct pid *owner,
			     const struct yz_module_policy_group *group)
{
	struct yz_module_policy_holder *holder;

	hash_for_each_possible(yz_module_policy_owners, holder, owner_node,
			       pid_nr(owner))
	{
		if (holder->pending && holder->watch.owner == owner &&
		    holder->group == group)
			return holder;
	}
	return NULL;
}

static int yz_merge_module_policy_state(struct ksu_file_load_policy *dst,
					const struct ksu_file_load_policy *src)
{
	if (!dst->src_type) {
		*dst = *src;
		return 0;
	}
	if (dst->src_type != src->src_type || dst->tgt_type != src->tgt_type)
		return -EINVAL;
	if (!!src->added_av != !!src->file_lease_refs ||
	    !!src->tmpfs_added_av != !!src->tmpfs_lease_refs ||
	    !!src->dir_added_av != !!src->dir_lease_refs)
		return -EINVAL;
	if ((src->added_av && (dst->target_class != src->target_class ||
			       dst->added_av != src->added_av)) ||
	    (src->tmpfs_added_av &&
	     (dst->tmpfs_type != src->tmpfs_type ||
	      dst->target_class != src->target_class ||
	      dst->tmpfs_added_av != src->tmpfs_added_av)) ||
	    (src->dir_added_av && (dst->dir_class != src->dir_class ||
				   dst->dir_added_av != src->dir_added_av)))
		return -EINVAL;
	if (U32_MAX - dst->file_lease_refs < src->file_lease_refs ||
	    U32_MAX - dst->tmpfs_lease_refs < src->tmpfs_lease_refs ||
	    U32_MAX - dst->dir_lease_refs < src->dir_lease_refs)
		return -EOVERFLOW;
	if (src->process_added_av) {
		if (!src->process_lease_refs)
			return -EINVAL;
		if (dst->process_added_av &&
		    (dst->process_type != src->process_type ||
		     dst->process_class != src->process_class ||
		     dst->process_added_av != src->process_added_av))
			return -EINVAL;
		if (!dst->process_added_av) {
			dst->process_type = src->process_type;
			dst->process_class = src->process_class;
		}
		if (U32_MAX - dst->process_lease_refs < src->process_lease_refs)
			return -EOVERFLOW;
	}
	dst->added_av |= src->added_av;
	dst->dir_added_av |= src->dir_added_av;
	dst->tmpfs_added_av |= src->tmpfs_added_av;
	dst->process_added_av |= src->process_added_av;
	dst->file_lease_refs += src->file_lease_refs;
	dst->tmpfs_lease_refs += src->tmpfs_lease_refs;
	dst->dir_lease_refs += src->dir_lease_refs;
	dst->process_lease_refs += src->process_lease_refs;
	return 0;
}

static struct yz_module_policy_group *
yz_put_module_policy_group_locked(struct yz_module_policy_group *group)
{
	if (--group->users)
		return NULL;
	yz_policy_retire(&group->policy);
	list_del_init(&group->list);
	return group;
}

static void yz_module_policy_detach(struct yz_module_policy_holder *holder,
				    struct list_head *todo)
{
	holder->pending = false;
	hash_del(&holder->owner_node);
	yz_policy_watch_del(&holder->watch);
	list_move_tail(&holder->list, todo);
}

static void yz_module_policy_free_holder(struct yz_module_policy_holder *holder)
{
	unsigned long flags;

	put_pid(holder->watch.owner);
	kfree(holder);
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.holders_current--;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	atomic_dec(&yz_policy_holders);
}

static void yz_module_policy_timeout(struct work_struct *work)
{
	struct yz_module_policy_holder *holder = container_of(
	    to_delayed_work(work), struct yz_module_policy_holder, timeout);
	struct yz_module_policy_group *group = NULL;
	LIST_HEAD(todo);
	bool release = false;

	mutex_lock(&yz_module_policy_lock);
	if (holder->pending) {
		yz_module_policy_detach(holder, &todo);
		list_del_init(&holder->list);
		group = yz_put_module_policy_group_locked(holder->group);
		release = true;
	}
	mutex_unlock(&yz_module_policy_lock);
	if (release)
		yz_module_policy_free_holder(holder);
	if (group)
		yz_policy_restore(&group->policy);
}

int ksu_yukizygisk_allow_module_load_policy(struct task_struct *task,
					    struct file *dir,
					    const struct cred *cred)
{
	struct yz_module_policy_group *group;
	struct yz_module_policy_holder *holder;
	struct yz_module_policy_group *new_group = NULL;
	struct yz_module_policy_holder *new_holder = NULL;
	struct pid *owner = NULL;
	unsigned long flags;
	int ret;
	bool schedule = false;

	if (!task || !dir || !cred)
		return -EINVAL;
	down_read(&yz_policy_admission);
	if (!yz_policy_enabled) {
		ret = -ESHUTDOWN;
		goto out;
	}
	rcu_read_lock();
	if (atomic_read(&task->signal->live) > 0)
		owner = get_pid(task_tgid(task));
	rcu_read_unlock();
	if (!owner) {
		ret = -ESRCH;
		goto out;
	}
	ret = yz_policy_reserve();
	if (ret)
		goto out;
	new_group = kzalloc(sizeof(*new_group), GFP_KERNEL);
	if (!new_group) {
		yz_policy_unreserve();
		ret = -ENOMEM;
		goto out;
	}
	new_group->policy.kind = YZ_POLICY_MODULE;
	INIT_LIST_HEAD(&new_group->list);
	INIT_LIST_HEAD(&new_group->policy.retry);
	if (atomic_inc_return(&yz_policy_holders) > YZ_POLICY_HOLDER_CAPACITY) {
		atomic_dec(&yz_policy_holders);
		ret = -ENOSPC;
		goto out;
	}
	new_holder = kzalloc(sizeof(*new_holder), GFP_KERNEL);
	if (!new_holder) {
		atomic_dec(&yz_policy_holders);
		ret = -ENOMEM;
		goto out;
	}
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.holders_current++;
	if (yz_policy_health.holders_peak < yz_policy_health.holders_current)
		yz_policy_health.holders_peak =
		    yz_policy_health.holders_current;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);

	mutex_lock(&yz_module_policy_lock);
	ret = ksu_file_load_policy_allow_cred(dir, cred,
					      &new_group->policy.state);
	if (ret)
		goto out_unlock;
	if (S_ISDIR(file_inode(dir)->i_mode)) {
		ret = ksu_file_load_policy_allow_execmem_cred(
		    cred, &new_group->policy.state);
		if (ret)
			goto out_unlock;
	}
	group = yz_find_module_policy_group(&new_group->policy.state);
	if (!group && !yz_policy_has_additions(&new_group->policy.state))
		goto out_unlock;
	if (group) {
		ret = yz_merge_module_policy_state(&group->policy.state,
						   &new_group->policy.state);
		if (ret)
			goto out_unlock;
		memset(&new_group->policy.state, 0,
		       sizeof(new_group->policy.state));
	} else {
		group = new_group;
		new_group = NULL;
		yz_policy_activate(&group->policy);
		list_add_tail(&group->list, &yz_module_policy_groups);
	}
	holder = yz_find_module_policy_holder(owner, group);
	if (!holder) {
		holder = new_holder;
		new_holder = NULL;
		INIT_LIST_HEAD(&holder->list);
		holder->group = group;
		holder->watch.owner = owner;
		owner = NULL;
		holder->pending = true;
		INIT_DELAYED_WORK(&holder->timeout, yz_module_policy_timeout);
		list_add_tail(&holder->list, &yz_module_policy_holders);
		hash_add(yz_module_policy_owners, &holder->owner_node,
			 pid_nr(holder->watch.owner));
		yz_policy_watch_add(&holder->watch);
		group->users++;
		schedule_delayed_work(&holder->timeout,
				      YZ_MODULE_POLICY_TIMEOUT);
	}
	if (!yz_policy_owner_alive(holder->watch.owner, true))
		schedule = yz_policy_queue_owner(holder->watch.owner);
out_unlock:
	mutex_unlock(&yz_module_policy_lock);
out:
	if (new_group)
		yz_policy_restore(&new_group->policy);
	if (new_holder)
		yz_module_policy_free_holder(new_holder);
	put_pid(owner);
	up_read(&yz_policy_admission);
	if (schedule)
		yz_process_exit_schedule();
	return ret;
}

static void yz_release_module_policies(struct list_head *holders)
{
	struct yz_module_policy_holder *holder, *tmp;
	struct yz_module_policy_group *group, *group_tmp;
	LIST_HEAD(groups);

	list_for_each_entry (holder, holders, list)
		cancel_delayed_work_sync(&holder->timeout);
	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, tmp, holders, list) {
		list_del_init(&holder->list);
		group = yz_put_module_policy_group_locked(holder->group);
		if (group)
			list_add_tail(&group->list, &groups);
		yz_module_policy_free_holder(holder);
	}
	mutex_unlock(&yz_module_policy_lock);
	list_for_each_entry_safe (group, group_tmp, &groups, list) {
		list_del_init(&group->list);
		yz_policy_restore(&group->policy);
	}
}

static void yz_restore_owner_policies(struct pid *owner)
{
	struct yz_native_policy_pending *entry;
	struct yz_module_policy_holder *holder;
	struct hlist_node *tmp;
	unsigned long flags;
	u64 visits = 0;
	LIST_HEAD(native);
	LIST_HEAD(modules);

	mutex_lock(&yz_native_policy_lock);
	hash_for_each_possible_safe(yz_native_policy_owners, entry, tmp,
				    owner_node, pid_nr(owner))
	{
		visits++;
		if (entry->watch.owner == owner)
			yz_native_policy_detach(entry, &native);
	}
	mutex_unlock(&yz_native_policy_lock);
	yz_release_native_policies(&native);
	mutex_lock(&yz_module_policy_lock);
	hash_for_each_possible_safe(yz_module_policy_owners, holder, tmp,
				    owner_node, pid_nr(owner))
	{
		visits++;
		if (holder->watch.owner == owner)
			yz_module_policy_detach(holder, &modules);
	}
	mutex_unlock(&yz_module_policy_lock);
	yz_release_module_policies(&modules);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.owner_cleanup_calls++;
	yz_cleanup_health.owner_index_visits += visits;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

int ksu_yukizygisk_restore_native_load_policy(pid_t tgid)
{
	struct pid *owner;

	if (tgid <= 0)
		return -EINVAL;
	owner = yz_policy_get_owner(tgid);
	if (owner) {
		yz_restore_owner_policies(owner);
		put_pid(owner);
	} else {
		yz_load_policy_reap(YZ_POLICY_REAP_MISSING_OWNER);
	}
	return 0;
}

void yz_load_policy_reap(enum yz_policy_reap_reason reason)
{
	struct yz_native_policy_pending *entry, *entry_tmp;
	struct yz_module_policy_holder *holder, *holder_tmp;
	unsigned long flags;
	u64 visits = 0;
	LIST_HEAD(native);
	LIST_HEAD(modules);

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	if (reason == YZ_POLICY_REAP_OVERFLOW)
		yz_cleanup_health.fullscan_overflow++;
	else if (reason == YZ_POLICY_REAP_ENABLE)
		yz_cleanup_health.fullscan_enable++;
	else
		yz_cleanup_health.fullscan_missing_owner++;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	mutex_lock(&yz_native_policy_lock);
	list_for_each_entry_safe (entry, entry_tmp, &yz_native_policy_pending,
				  list) {
		visits++;
		if (!yz_policy_owner_alive(entry->watch.owner, false))
			yz_native_policy_detach(entry, &native);
	}
	mutex_unlock(&yz_native_policy_lock);
	yz_release_native_policies(&native);
	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, holder_tmp, &yz_module_policy_holders,
				  list) {
		visits++;
		if (!yz_policy_owner_alive(holder->watch.owner, false))
			yz_module_policy_detach(holder, &modules);
	}
	mutex_unlock(&yz_module_policy_lock);
	yz_release_module_policies(&modules);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.fullscan_entries += visits;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

void yz_load_policy_drain_exits(void)
{
	struct pid *owners[YZ_POLICY_EXIT_CAPACITY];
	unsigned long flags;
	unsigned int count, i;
	bool reconcile;

	for (;;) {
		spin_lock_irqsave(&yz_policy_watch_lock, flags);
		count = yz_policy_exit_count;
		memcpy(owners, yz_policy_exit_owners, count * sizeof(*owners));
		yz_policy_exit_count = 0;
		yz_cleanup_health.inflight_owners += count;
		reconcile = yz_policy_reconcile_required;
		yz_policy_reconcile_required = false;
		spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
		if (!count && !reconcile)
			break;
		if (reconcile)
			yz_load_policy_reap(YZ_POLICY_REAP_OVERFLOW);
		for (i = 0; i < count; i++) {
			if (!reconcile)
				yz_restore_owner_policies(owners[i]);
			put_pid(owners[i]);
			spin_lock_irqsave(&yz_policy_watch_lock, flags);
			yz_cleanup_health.inflight_owners--;
			spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
		}
	}
}

void yz_cleanup_module_policies(void)
{
	struct yz_native_policy_pending *entry, *entry_tmp;
	struct yz_module_policy_holder *holder, *holder_tmp;
	unsigned long flags;
	u64 visits = 0;
	LIST_HEAD(native);
	LIST_HEAD(modules);

	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.fullscan_disable++;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	mutex_lock(&yz_native_policy_lock);
	list_for_each_entry_safe (entry, entry_tmp, &yz_native_policy_pending,
				  list) {
		visits++;
		yz_native_policy_detach(entry, &native);
	}
	mutex_unlock(&yz_native_policy_lock);
	yz_release_native_policies(&native);
	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, holder_tmp, &yz_module_policy_holders,
				  list) {
		visits++;
		yz_module_policy_detach(holder, &modules);
	}
	mutex_unlock(&yz_module_policy_lock);
	yz_release_module_policies(&modules);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_cleanup_health.fullscan_entries += visits;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
}

void yz_load_policy_enable(void)
{
	unsigned long flags;

	down_write(&yz_policy_admission);
	yz_policy_enabled = true;
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.enabled = true;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_policy_exit_accepting = true;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	up_write(&yz_policy_admission);
}

void yz_load_policy_disable(void)
{
	unsigned long flags;

	down_write(&yz_policy_admission);
	yz_policy_enabled = false;
	spin_lock_irqsave(&yz_policy_health_lock, flags);
	yz_policy_health.enabled = false;
	spin_unlock_irqrestore(&yz_policy_health_lock, flags);
	spin_lock_irqsave(&yz_policy_watch_lock, flags);
	yz_policy_exit_accepting = false;
	spin_unlock_irqrestore(&yz_policy_watch_lock, flags);
	yz_load_policy_drain_exits();
	yz_cleanup_module_policies();
	up_write(&yz_policy_admission);
}

void yz_load_policy_exit(void)
{
	yz_load_policy_disable();
	cancel_delayed_work_sync(&yz_policy_retry_work);
	WARN_ON(atomic_read(&yz_policy_states));
}
