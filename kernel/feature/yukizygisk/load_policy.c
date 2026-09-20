#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "api.h"
#include "internal.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep

#define YZ_NATIVE_POLICY_TIMEOUT (10 * HZ)
#define YZ_MODULE_POLICY_TIMEOUT (10 * HZ)

struct yz_native_policy_pending {
	struct list_head list;
	pid_t tgid;
	struct ksu_file_load_policy state;
	struct delayed_work timeout;
	bool pending;
};

static DEFINE_MUTEX(yz_native_policy_lock);
static LIST_HEAD(yz_native_policy_pending);

struct yz_module_policy_group {
	struct list_head list;
	struct ksu_file_load_policy state;
	u32 users;
};

struct yz_module_policy_holder {
	struct list_head list;
	struct yz_module_policy_group *group;
	struct delayed_work timeout;
	pid_t tgid;
	bool pending;
};

static DEFINE_MUTEX(yz_module_policy_lock);
static LIST_HEAD(yz_module_policy_groups);
static LIST_HEAD(yz_module_policy_holders);

static int yz_restore_module_policy(pid_t tgid);

static bool
yz_native_policy_has_additions(const struct ksu_file_load_policy *state)
{
	return state && (state->added_av || state->tmpfs_added_av ||
			 state->process_added_av || state->dir_added_av);
}

void yz_restore_native_policy_state(struct ksu_file_load_policy *state)
{
	if (!yz_native_policy_has_additions(state))
		return;
	ksu_file_load_policy_restore(state);
	memset(state, 0, sizeof(*state));
}

static void yz_native_policy_timeout(struct work_struct *work)
{
	struct yz_native_policy_pending *entry = container_of(
	    to_delayed_work(work), struct yz_native_policy_pending, timeout);
	bool restore = false;

	mutex_lock(&yz_native_policy_lock);
	if (entry->pending) {
		entry->pending = false;
		list_del_init(&entry->list);
		restore = true;
	}
	mutex_unlock(&yz_native_policy_lock);

	if (!restore)
		return;

	pr_info("yukizygisk: native load policy expired pid=%d file=0x%x "
		"tmpfs=0x%x process=0x%x\n",
		entry->tgid, entry->state.added_av, entry->state.tmpfs_added_av,
		entry->state.process_added_av);
	yz_restore_native_policy_state(&entry->state);
	kfree(entry);
}

void yz_publish_native_policy_state(pid_t tgid,
				    struct ksu_file_load_policy *state)
{
	struct yz_native_policy_pending *entry;
	struct yz_native_policy_pending *cur;
	struct yz_native_policy_pending *tmp;
	LIST_HEAD(old_entries);

	if (!yz_native_policy_has_additions(state))
		return;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		pr_info("yukizygisk: native load policy allocation failed "
			"pid=%d; restoring\n",
			tgid);
		yz_restore_native_policy_state(state);
		return;
	}
	entry->tgid = tgid;
	entry->state = *state;
	entry->pending = true;
	INIT_LIST_HEAD(&entry->list);
	INIT_DELAYED_WORK(&entry->timeout, yz_native_policy_timeout);
	memset(state, 0, sizeof(*state));

	mutex_lock(&yz_native_policy_lock);
	list_for_each_entry_safe (cur, tmp, &yz_native_policy_pending, list) {
		if (cur->tgid != tgid)
			continue;
		cur->pending = false;
		list_move_tail(&cur->list, &old_entries);
	}
	list_add_tail(&entry->list, &yz_native_policy_pending);
	mutex_unlock(&yz_native_policy_lock);

	schedule_delayed_work(&entry->timeout, YZ_NATIVE_POLICY_TIMEOUT);

	list_for_each_entry_safe (cur, tmp, &old_entries, list) {
		cancel_delayed_work_sync(&cur->timeout);
		list_del(&cur->list);
		yz_restore_native_policy_state(&cur->state);
		kfree(cur);
	}
	pr_info("yukizygisk: native load policy enabled pid=%d file=0x%x "
		"tmpfs=0x%x process=0x%x\n",
		tgid, entry->state.added_av, entry->state.tmpfs_added_av,
		entry->state.process_added_av);
}

int ksu_yukizygisk_restore_native_load_policy(pid_t tgid)
{
	struct yz_native_policy_pending *entry;
	struct yz_native_policy_pending *tmp;
	LIST_HEAD(todo);
	int n = 0;

	if (tgid <= 0)
		return -EINVAL;

	mutex_lock(&yz_native_policy_lock);
	list_for_each_entry_safe (entry, tmp, &yz_native_policy_pending, list) {
		if (entry->tgid != tgid)
			continue;
		entry->pending = false;
		list_move_tail(&entry->list, &todo);
	}
	mutex_unlock(&yz_native_policy_lock);

	list_for_each_entry_safe (entry, tmp, &todo, list) {
		cancel_delayed_work_sync(&entry->timeout);
		list_del(&entry->list);
		yz_restore_native_policy_state(&entry->state);
		kfree(entry);
		n++;
	}
	pr_info("yukizygisk: native load policy restored pid=%d entries=%d\n",
		tgid, n);
	return yz_restore_module_policy(tgid);
}

static struct yz_module_policy_group *
yz_find_module_policy_group(const struct ksu_file_load_policy *state)
{
	struct yz_module_policy_group *group;

	list_for_each_entry (group, &yz_module_policy_groups, list) {
		const struct ksu_file_load_policy *cur = &group->state;

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
yz_find_module_policy_holder(pid_t tgid,
			     const struct yz_module_policy_group *group)
{
	struct yz_module_policy_holder *holder;

	list_for_each_entry (holder, &yz_module_policy_holders, list) {
		if (holder->pending && holder->tgid == tgid &&
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

static void
yz_put_module_policy_group_locked(struct yz_module_policy_group *group)
{
	int ret;

	if (!group || !group->users)
		return;
	if (--group->users)
		return;

	list_del(&group->list);
	ret = ksu_file_load_policy_restore(&group->state);
	pr_info("yukizygisk: module load policy released source=%u target=%u "
		"err=%d\n",
		group->state.src_type, group->state.tgt_type, ret);
	kfree(group);
}

static void yz_module_policy_timeout(struct work_struct *work)
{
	struct yz_module_policy_holder *holder = container_of(
	    to_delayed_work(work), struct yz_module_policy_holder, timeout);
	bool release = false;

	mutex_lock(&yz_module_policy_lock);
	if (holder->pending) {
		holder->pending = false;
		list_del_init(&holder->list);
		pr_info("yukizygisk: module load policy expired pid=%d\n",
			holder->tgid);
		yz_put_module_policy_group_locked(holder->group);
		release = true;
	}
	mutex_unlock(&yz_module_policy_lock);

	if (release)
		kfree(holder);
}

int ksu_yukizygisk_allow_module_load_policy(pid_t tgid, struct file *dir,
					    const struct cred *cred)
{
	struct yz_module_policy_group *group;
	struct yz_module_policy_holder *holder;
	struct yz_module_policy_group *new_group;
	struct yz_module_policy_holder *new_holder;
	struct ksu_file_load_policy state = {0};
	int restore_ret;
	int ret;

	if (tgid <= 0 || !dir || !cred)
		return -EINVAL;

	new_group = kzalloc(sizeof(*new_group), GFP_KERNEL);
	new_holder = kzalloc(sizeof(*new_holder), GFP_KERNEL);
	if (!new_group || !new_holder) {
		kfree(new_group);
		kfree(new_holder);
		return -ENOMEM;
	}

	mutex_lock(&yz_module_policy_lock);
	ret = ksu_file_load_policy_allow_cred(dir, cred, &state);
	if (ret)
		goto out_unlock;
	if (S_ISDIR(file_inode(dir)->i_mode)) {
		ret = ksu_file_load_policy_allow_execmem_cred(cred, &state);
		if (ret)
			goto out_restore;
	}

	group = yz_find_module_policy_group(&state);
	if (!group && !yz_native_policy_has_additions(&state))
		goto out_unlock;
	if (!group) {
		group = new_group;
		new_group = NULL;
		INIT_LIST_HEAD(&group->list);
		group->state = state;
		list_add_tail(&group->list, &yz_module_policy_groups);
	} else {
		ret = yz_merge_module_policy_state(&group->state, &state);
		if (ret)
			goto out_restore;
	}
	memset(&state, 0, sizeof(state));

	holder = yz_find_module_policy_holder(tgid, group);
	if (holder)
		goto out_unlock;

	holder = new_holder;
	new_holder = NULL;
	INIT_LIST_HEAD(&holder->list);
	holder->group = group;
	holder->tgid = tgid;
	holder->pending = true;
	INIT_DELAYED_WORK(&holder->timeout, yz_module_policy_timeout);
	list_add_tail(&holder->list, &yz_module_policy_holders);
	group->users++;
	schedule_delayed_work(&holder->timeout, YZ_MODULE_POLICY_TIMEOUT);
	pr_info("yukizygisk: module load policy enabled pid=%d source=%u "
		"target=%u file=0x%x dir=0x%x tmpfs=0x%x process=0x%x\n",
		tgid, group->state.src_type, group->state.tgt_type,
		group->state.added_av, group->state.dir_added_av,
		group->state.tmpfs_added_av, group->state.process_added_av);

	goto out_unlock;

out_restore:
	restore_ret = ksu_file_load_policy_restore(&state);
	if (restore_ret)
		pr_err("yukizygisk: module load policy restore failed pid=%d "
		       "err=%d\n",
		       tgid, restore_ret);

out_unlock:
	mutex_unlock(&yz_module_policy_lock);
	kfree(new_group);
	kfree(new_holder);
	return ret;
}

static int yz_restore_module_policy(pid_t tgid)
{
	struct yz_module_policy_holder *holder;
	struct yz_module_policy_holder *tmp;
	LIST_HEAD(todo);
	int n = 0;

	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, tmp, &yz_module_policy_holders,
				  list) {
		if (!holder->pending || holder->tgid != tgid)
			continue;
		holder->pending = false;
		list_move_tail(&holder->list, &todo);
	}
	mutex_unlock(&yz_module_policy_lock);

	list_for_each_entry (holder, &todo, list)
		cancel_delayed_work_sync(&holder->timeout);

	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, tmp, &todo, list) {
		list_del(&holder->list);
		yz_put_module_policy_group_locked(holder->group);
		kfree(holder);
		n++;
	}
	mutex_unlock(&yz_module_policy_lock);

	if (n)
		pr_info("yukizygisk: module load policy restored pid=%d "
			"entries=%d\n",
			tgid, n);
	return 0;
}

void yz_cleanup_module_policies(void)
{
	struct yz_module_policy_holder *holder;
	struct yz_module_policy_holder *tmp;
	LIST_HEAD(todo);

	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, tmp, &yz_module_policy_holders,
				  list) {
		holder->pending = false;
		list_move_tail(&holder->list, &todo);
	}
	mutex_unlock(&yz_module_policy_lock);

	list_for_each_entry (holder, &todo, list)
		cancel_delayed_work_sync(&holder->timeout);

	mutex_lock(&yz_module_policy_lock);
	list_for_each_entry_safe (holder, tmp, &todo, list) {
		list_del(&holder->list);
		yz_put_module_policy_group_locked(holder->group);
		kfree(holder);
	}
	mutex_unlock(&yz_module_policy_lock);
}
