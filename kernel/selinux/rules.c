#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "klog.h" // IWYU pragma: keep
#include "linux/lsm_audit.h" // IWYU pragma: keep
#include "objsec.h"
#include "selinux.h"
#include "sepolicy.h"
#include "ss/context.h"
#include "ss/services.h"
#include "security.h"
#include "uapi/selinux.h"
#include "xfrm.h"

#define SELINUX_POLICY_INSTEAD_SELINUX_SS

#define ALL NULL

struct selinux_policy *backup_sepolicy;

static struct policydb *get_policydb(void)
{
	struct policydb *db;
	struct selinux_policy *policy = selinux_state.policy;
	db = &policy->policydb;
	return db;
}

static DEFINE_MUTEX(ksu_rules);

static void reset_avc_cache(void);

static void backup_original_sepolicy_once(void)
{
	int ret;

	if (backup_sepolicy)
		return;

	backup_sepolicy = ksu_dup_sepolicy(selinux_state.policy);
	if (IS_ERR(backup_sepolicy)) {
		pr_err("failed to create backup sepolicy: %ld\n",
		       PTR_ERR(backup_sepolicy));
		backup_sepolicy = NULL;
		return;
	}

	backup_sepolicy->sidtab =
	    kzalloc(sizeof(*backup_sepolicy->sidtab), GFP_KERNEL);
	if (!backup_sepolicy->sidtab) {
		pr_err("failed to alloc backup sidtab\n");
		ksu_destroy_sepolicy(backup_sepolicy);
		backup_sepolicy = NULL;
		return;
	}

	ret = policydb_load_isids(&backup_sepolicy->policydb,
				  backup_sepolicy->sidtab);
	if (ret) {
		pr_err("failed to load isids for backup sepolicy: %d\n", ret);
		kfree(backup_sepolicy->sidtab);
		ksu_destroy_sepolicy(backup_sepolicy);
		backup_sepolicy = NULL;
		return;
	}

	pr_info("backup sepolicy success\n");
}

static const char *ksu_file_load_perms[] = {
    "read", "open", "getattr", "map", "execute",
};

static const char *ksu_dir_load_perms[] = {
    "read",
    "open",
    "getattr",
    "search",
};

static const char *ksu_tmpfs_hook_perms[] = {
    "read", "open", "getattr", "map", "execute",
};

/* Read-only SCM_RIGHTS handoff and source mapping from su-domain zygiskd. */
static const char *ksu_tmpfs_receive_perms[] = {
    "read",
    "open",
    "getattr",
    "map",
};

static const char *ksu_process_execmem_perms[] = {
    "execmem",
};

struct ksu_process_policy_lease {
	struct list_head list;
	u32 process_type;
	u16 process_class;
	u32 added_av;
	u32 users;
};

static LIST_HEAD(ksu_process_policy_leases);

static struct ksu_process_policy_lease *
ksu_find_process_policy_lease(u32 process_type, u16 process_class)
{
	struct ksu_process_policy_lease *lease;

	list_for_each_entry (lease, &ksu_process_policy_leases, list) {
		if (lease->process_type == process_type &&
		    lease->process_class == process_class)
			return lease;
	}
	return NULL;
}

static u32 ksu_file_perm_mask(struct class_datum *cls, const char *perm_name)
{
	struct perm_datum *perm;

	if (!cls || !perm_name)
		return 0;

	perm = symtab_search(&cls->permissions, perm_name);
	if (!perm && cls->comdatum)
		perm = symtab_search(&cls->comdatum->permissions, perm_name);
	if (!perm || perm->value == 0 || perm->value > 32)
		return 0;
	return 1U << (perm->value - 1);
}

static u32 ksu_required_av(struct class_datum *cls, const char *const *perms,
			   int count)
{
	u32 av = 0;
	int i;

	for (i = 0; i < count; i++)
		av |= ksu_file_perm_mask(cls, perms[i]);
	return av;
}

static u32 ksu_direct_allowed_av(struct policydb *db, u32 src_type,
				 u32 tgt_type, u16 target_class)
{
	struct avtab_key key = {};
	struct avtab_node *node;

	key.source_type = src_type;
	key.target_type = tgt_type;
	key.target_class = target_class;
	key.specified = AVTAB_ALLOWED;
	node = avtab_search_node(&db->te_avtab, &key);
	return node ? node->datum.u.data : 0;
}

#define KSU_TEMP_AV_RULE_MAX 64
#define KSU_TEMP_AV_BITS 32

struct ksu_temp_av_key {
	u32 src_type;
	u32 tgt_type;
	u16 tclass;
};

struct ksu_temp_av_rule {
	bool used;
	struct ksu_temp_av_key key;
	u32 refs[KSU_TEMP_AV_BITS];
};

static struct ksu_temp_av_rule ksu_temp_av_rules[KSU_TEMP_AV_RULE_MAX];

static bool ksu_temp_av_key_eq(const struct ksu_temp_av_key *a,
			       const struct ksu_temp_av_key *b)
{
	return a->src_type == b->src_type && a->tgt_type == b->tgt_type &&
	       a->tclass == b->tclass;
}

static struct ksu_temp_av_rule *
ksu_temp_av_find_locked(const struct ksu_temp_av_key *key)
{
	int i;

	for (i = 0; i < KSU_TEMP_AV_RULE_MAX; i++) {
		if (ksu_temp_av_rules[i].used &&
		    ksu_temp_av_key_eq(&ksu_temp_av_rules[i].key, key))
			return &ksu_temp_av_rules[i];
	}
	return NULL;
}

static struct ksu_temp_av_rule *ksu_temp_av_find_free_locked(void)
{
	int i;

	for (i = 0; i < KSU_TEMP_AV_RULE_MAX; i++) {
		if (!ksu_temp_av_rules[i].used)
			return &ksu_temp_av_rules[i];
	}
	return NULL;
}

static int ksu_temp_av_free_count_locked(void)
{
	int count = 0;
	int i;

	for (i = 0; i < KSU_TEMP_AV_RULE_MAX; i++) {
		if (!ksu_temp_av_rules[i].used)
			count++;
	}
	return count;
}

static u32 ksu_temp_av_mask_locked(const struct ksu_temp_av_key *key)
{
	struct ksu_temp_av_rule *rule = ksu_temp_av_find_locked(key);
	u32 mask = 0;
	int i;

	if (!rule)
		return 0;
	for (i = 0; i < KSU_TEMP_AV_BITS; i++) {
		if (rule->refs[i])
			mask |= 1U << i;
	}
	return mask;
}

static int ksu_temp_av_validate_add_locked(const struct ksu_temp_av_key *key,
					   u32 av, u32 refs)
{
	struct ksu_temp_av_rule *rule;
	int i;

	if (!av || !refs)
		return 0;
	rule = ksu_temp_av_find_locked(key);
	if (!rule && !ksu_temp_av_find_free_locked())
		return -ENOSPC;
	if (!rule)
		return 0;
	for (i = 0; i < KSU_TEMP_AV_BITS; i++) {
		if ((av & (1U << i)) && U32_MAX - rule->refs[i] < refs)
			return -EOVERFLOW;
	}
	return 0;
}

static void ksu_temp_av_add_locked(const struct ksu_temp_av_key *key, u32 av,
				   u32 refs)
{
	struct ksu_temp_av_rule *rule;
	int i;

	if (!av || !refs)
		return;
	rule = ksu_temp_av_find_locked(key);
	if (!rule) {
		rule = ksu_temp_av_find_free_locked();
		if (!rule)
			return;
		memset(rule, 0, sizeof(*rule));
		rule->used = true;
		rule->key = *key;
	}
	for (i = 0; i < KSU_TEMP_AV_BITS; i++) {
		if (av & (1U << i))
			rule->refs[i] += refs;
	}
}

static int ksu_temp_av_plan_release_locked(const struct ksu_temp_av_key *key,
					   u32 av, u32 refs, u32 *clear_av)
{
	struct ksu_temp_av_rule *rule;
	u32 clear = 0;
	int i;

	if (clear_av)
		*clear_av = 0;
	if (!av || !refs)
		return 0;
	rule = ksu_temp_av_find_locked(key);
	if (!rule)
		return -ENOENT;
	for (i = 0; i < KSU_TEMP_AV_BITS; i++) {
		if (!(av & (1U << i)))
			continue;
		if (rule->refs[i] < refs)
			return -EINVAL;
		if (rule->refs[i] == refs)
			clear |= 1U << i;
	}
	if (clear_av)
		*clear_av = clear;
	return 0;
}

static void ksu_temp_av_release_locked(const struct ksu_temp_av_key *key,
				       u32 av, u32 refs)
{
	struct ksu_temp_av_rule *rule;
	bool any = false;
	int i;

	if (!av || !refs)
		return;
	rule = ksu_temp_av_find_locked(key);
	if (!rule)
		return;
	for (i = 0; i < KSU_TEMP_AV_BITS; i++) {
		if (av & (1U << i))
			rule->refs[i] -= refs;
		if (rule->refs[i])
			any = true;
	}
	if (!any)
		memset(rule, 0, sizeof(*rule));
}

static int ksu_temp_av_plan_allow_locked(struct policydb *db,
					 const struct ksu_temp_av_key *key,
					 u32 required_av, u32 *held_av,
					 u32 *commit_av)
{
	u32 direct_av;
	u32 owned_av;
	u32 held;
	int ret;

	if (held_av)
		*held_av = 0;
	if (commit_av)
		*commit_av = 0;
	if (!required_av)
		return 0;
	direct_av = ksu_direct_allowed_av(db, key->src_type, key->tgt_type,
					  key->tclass);
	owned_av = ksu_temp_av_mask_locked(key);
	held = required_av & (~direct_av | owned_av);
	ret = ksu_temp_av_validate_add_locked(key, held, 1);
	if (ret)
		return ret;
	if (held_av)
		*held_av = held;
	if (commit_av)
		*commit_av = held & ~owned_av;
	return 0;
}

static const char *ksu_type_name_by_value(struct policydb *db, u32 type)
{
	if (!db || type == 0 || type > db->p_types.nprim)
		return NULL;
	if (!db->sym_val_to_name[SYM_TYPES])
		return NULL;
	return db->sym_val_to_name[SYM_TYPES][type - 1];
}

static u32 ksu_type_value_by_name(struct policydb *db, const char *name)
{
	struct type_datum *type;

	if (!db || !name)
		return 0;
	type = symtab_search(&db->p_types, name);
	return type ? type->value : 0;
}

static bool ksu_apply_file_av(struct policydb *db, const char *src,
			      const char *tgt, u32 av, bool allow,
			      const char *const *perms, int count)
{
	struct class_datum *cls;
	bool ok = true;
	int i;

	cls = symtab_search(&db->p_classes, "file");
	if (!cls)
		return false;

	for (i = 0; i < count; i++) {
		u32 mask = ksu_file_perm_mask(cls, perms[i]);

		if (!(av & mask))
			continue;
		if (allow)
			ok = ksu_allow(db, src, tgt, "file", perms[i]) && ok;
		else
			ok = ksu_deny(db, src, tgt, "file", perms[i]) && ok;
	}
	return ok;
}

static bool ksu_apply_dir_av(struct policydb *db, const char *src,
			     const char *tgt, u32 av, bool allow)
{
	struct class_datum *cls;
	bool ok = true;
	int i;

	cls = symtab_search(&db->p_classes, "dir");
	if (!cls)
		return false;

	for (i = 0; i < ARRAY_SIZE(ksu_dir_load_perms); i++) {
		u32 mask = ksu_file_perm_mask(cls, ksu_dir_load_perms[i]);

		if (!(av & mask))
			continue;
		if (allow)
			ok = ksu_allow(db, src, tgt, "dir",
				       ksu_dir_load_perms[i]) &&
			     ok;
		else
			ok = ksu_deny(db, src, tgt, "dir",
				      ksu_dir_load_perms[i]) &&
			     ok;
	}
	return ok;
}

static bool ksu_apply_process_av(struct policydb *db, const char *src, u32 av,
				 bool allow)
{
	struct class_datum *cls;
	bool ok = true;
	int i;

	cls = symtab_search(&db->p_classes, "process");
	if (!cls)
		return false;

	for (i = 0; i < ARRAY_SIZE(ksu_process_execmem_perms); i++) {
		const char *perm = ksu_process_execmem_perms[i];
		u32 mask = ksu_file_perm_mask(cls, perm);

		if (!(av & mask))
			continue;
		if (allow)
			ok = ksu_allow(db, src, src, "process", perm) && ok;
		else
			ok = ksu_deny(db, src, src, "process", perm) && ok;
	}
	return ok;
}

static int ksu_file_load_policy_allow_sid(struct file *file, u32 ssid,
					  bool include_dir, bool receive_only,
					  const char *const *tmpfs_perms,
					  int tmpfs_perm_count,
					  struct ksu_file_load_policy *state)
{
	struct selinux_policy *pol, *old_pol;
	struct policydb *db;
	struct inode_security_struct *isec;
	struct context *scontext;
	struct context *tcontext;
	struct class_datum *cls;
	struct class_datum *dir_cls = NULL;
	struct ksu_temp_av_key file_key = {};
	struct ksu_temp_av_key tmpfs_key = {};
	struct ksu_temp_av_key dir_key = {};
	const char *src_name;
	const char *tgt_name;
	char src_log[64];
	char tgt_log[64];
	u32 tsid;
	u32 required_av;
	u32 add_av = 0;
	u32 file_commit_av = 0;
	u32 tmpfs_type;
	u32 tmpfs_required_av;
	u32 tmpfs_add_av = 0;
	u32 tmpfs_commit_av = 0;
	u32 dir_required_av = 0;
	u32 dir_add_av = 0;
	u32 dir_commit_av = 0;
	bool commit_policy;
	int needed_slots = 0;
	int ret = 0;

	if (!file || !state)
		return -EINVAL;
	if (state->added_av || state->tmpfs_added_av || state->dir_added_av ||
	    state->process_added_av || state->file_lease_refs ||
	    state->tmpfs_lease_refs || state->dir_lease_refs ||
	    state->process_lease_refs)
		return -EBUSY;
	memset(state, 0, sizeof(*state));

	isec = selinux_inode(file_inode(file));
	if (!isec)
		return -EINVAL;
	tsid = isec->sid;
	if (!ssid || !tsid)
		return -EINVAL;

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	db = &old_pol->policydb;
	scontext = sidtab_search(old_pol->sidtab, ssid);
	tcontext = sidtab_search(old_pol->sidtab, tsid);
	if (!scontext || !tcontext) {
		ret = -ENOENT;
		goto out_unlock;
	}
	cls = symtab_search(&db->p_classes, "file");
	if (!cls) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (include_dir) {
		dir_cls = symtab_search(&db->p_classes, "dir");
		if (!dir_cls) {
			ret = -ENOENT;
			goto out_unlock;
		}
	}
	src_name = ksu_type_name_by_value(db, scontext->type);
	tgt_name = ksu_type_name_by_value(db, tcontext->type);
	if (!src_name || !tgt_name) {
		ret = -ENOENT;
		goto out_unlock;
	}
	strscpy(src_log, src_name, sizeof(src_log));
	strscpy(tgt_log, tgt_name, sizeof(tgt_log));

	required_av = receive_only
			  ? ksu_required_av(cls, ksu_tmpfs_receive_perms,
					    ARRAY_SIZE(ksu_tmpfs_receive_perms))
			  : ksu_required_av(cls, ksu_file_load_perms,
					    ARRAY_SIZE(ksu_file_load_perms));
	if (dir_cls) {
		dir_required_av =
		    ksu_required_av(dir_cls, ksu_dir_load_perms,
				    ARRAY_SIZE(ksu_dir_load_perms));
	}

	tmpfs_type = tmpfs_perms && tmpfs_perm_count > 0
			 ? ksu_type_value_by_name(db, "tmpfs")
			 : 0;
	if (tmpfs_type) {
		tmpfs_required_av =
		    ksu_required_av(cls, tmpfs_perms, tmpfs_perm_count);
		if (tmpfs_type == tcontext->type) {
			/* The tmpfs role is a superset; acquire the key only
			 * once. */
			required_av = 0;
		}
	}

	file_key.src_type = scontext->type;
	file_key.tgt_type = tcontext->type;
	file_key.tclass = cls->value;
	ret = ksu_temp_av_plan_allow_locked(db, &file_key, required_av, &add_av,
					    &file_commit_av);
	if (ret)
		goto out_unlock;
	if (dir_cls) {
		dir_key.src_type = scontext->type;
		dir_key.tgt_type = tcontext->type;
		dir_key.tclass = dir_cls->value;
		ret = ksu_temp_av_plan_allow_locked(
		    db, &dir_key, dir_required_av, &dir_add_av, &dir_commit_av);
		if (ret)
			goto out_unlock;
	}
	if (tmpfs_type) {
		tmpfs_key.src_type = scontext->type;
		tmpfs_key.tgt_type = tmpfs_type;
		tmpfs_key.tclass = cls->value;
		ret = ksu_temp_av_plan_allow_locked(
		    db, &tmpfs_key, tmpfs_required_av, &tmpfs_add_av,
		    &tmpfs_commit_av);
		if (ret)
			goto out_unlock;
	}
	if (add_av && !ksu_temp_av_find_locked(&file_key))
		needed_slots++;
	if (dir_add_av && !ksu_temp_av_find_locked(&dir_key))
		needed_slots++;
	if (tmpfs_add_av && !ksu_temp_av_find_locked(&tmpfs_key))
		needed_slots++;
	if (needed_slots > ksu_temp_av_free_count_locked()) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	state->src_type = scontext->type;
	state->tgt_type = tcontext->type;
	state->tmpfs_type = tmpfs_type;
	state->target_class = cls->value;
	state->dir_class = dir_cls ? dir_cls->value : 0;
	state->added_av = add_av;
	state->tmpfs_added_av = tmpfs_add_av;
	state->dir_added_av = dir_add_av;
	state->file_lease_refs = add_av ? 1 : 0;
	state->tmpfs_lease_refs = tmpfs_add_av ? 1 : 0;
	state->dir_lease_refs = dir_add_av ? 1 : 0;

	if (!add_av && !tmpfs_add_av && !dir_add_av)
		goto out_unlock;
	commit_policy = file_commit_av || tmpfs_commit_av || dir_commit_av;
	if (!commit_policy)
		goto add_leases;

	pol = ksu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("file_load_policy: dup failed: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;
	if (file_commit_av &&
	    !ksu_apply_file_av(db, src_name, tgt_name, file_commit_av, true,
			       ksu_file_load_perms,
			       ARRAY_SIZE(ksu_file_load_perms))) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (tmpfs_commit_av &&
	    !ksu_apply_file_av(db, src_name, "tmpfs", tmpfs_commit_av, true,
			       tmpfs_perms, tmpfs_perm_count)) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (dir_commit_av &&
	    !ksu_apply_dir_av(db, src_name, tgt_name, dir_commit_av, true)) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}

	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	ksu_destroy_sepolicy(old_pol);
	reset_avc_cache();

add_leases:
	ksu_temp_av_add_locked(&file_key, add_av, 1);
	ksu_temp_av_add_locked(&dir_key, dir_add_av, 1);
	ksu_temp_av_add_locked(&tmpfs_key, tmpfs_add_av, 1);
	pr_info("file_load_policy: allow src=%s tgt=%s file=0x%x "
		"dir=0x%x tmpfs=0x%x committed=0x%x/0x%x/0x%x\n",
		src_log, tgt_log, add_av, dir_add_av, tmpfs_add_av,
		file_commit_av, dir_commit_av, tmpfs_commit_av);

out_unlock:
	if (ret)
		memset(state, 0, sizeof(*state));
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

int ksu_file_load_policy_allow_current(struct file *file,
				       struct ksu_file_load_policy *state)
{
	return ksu_file_load_policy_allow_sid(
	    file, current_sid(), false, false, ksu_tmpfs_hook_perms,
	    ARRAY_SIZE(ksu_tmpfs_hook_perms), state);
}

static u32 ksu_file_load_policy_cred_sid(const struct cred *cred)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
	const struct task_security_struct *tsec;
#else
	const struct cred_security_struct *tsec;
#endif // #if LINUX_VERSION_CODE < KERNEL_VERSION...

	if (!cred)
		return 0;
	tsec = selinux_cred(cred);
	return tsec ? tsec->sid : 0;
}

int ksu_file_load_policy_allow_cred(struct file *file, const struct cred *cred,
				    struct ksu_file_load_policy *state)
{
	u32 sid = ksu_file_load_policy_cred_sid(cred);

	if (!sid || !file)
		return -EINVAL;
	if (!S_ISDIR(file_inode(file)->i_mode))
		return ksu_file_load_policy_allow_sid(file, sid, false, true,
						      NULL, 0, state);
	return ksu_file_load_policy_allow_sid(
	    file, sid, true, false, ksu_tmpfs_receive_perms,
	    ARRAY_SIZE(ksu_tmpfs_receive_perms), state);
}

static int
ksu_file_load_policy_allow_execmem_sid(u32 ssid,
				       struct ksu_file_load_policy *state)
{
	struct selinux_policy *pol, *old_pol;
	struct ksu_process_policy_lease *lease;
	struct ksu_process_policy_lease *new_lease = NULL;
	struct policydb *db;
	struct context *scontext;
	struct class_datum *cls;
	const char *src_name;
	u32 required_av;
	u32 direct_av;
	u32 add_av;
	int ret = 0;

	if (!state || !ssid)
		return -EINVAL;

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	db = &old_pol->policydb;
	scontext = sidtab_search(old_pol->sidtab, ssid);
	if (!scontext) {
		ret = -ENOENT;
		goto out_unlock;
	}
	cls = symtab_search(&db->p_classes, "process");
	if (!cls) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if ((state->src_type && state->src_type != scontext->type) ||
	    (state->process_lease_refs &&
	     (state->process_type != scontext->type ||
	      state->process_class != cls->value))) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (!!state->process_added_av != !!state->process_lease_refs) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (state->process_lease_refs)
		goto out_unlock;
	src_name = ksu_type_name_by_value(db, scontext->type);
	if (!src_name) {
		ret = -ENOENT;
		goto out_unlock;
	}

	required_av = ksu_required_av(cls, ksu_process_execmem_perms,
				      ARRAY_SIZE(ksu_process_execmem_perms));
	lease = ksu_find_process_policy_lease(scontext->type, cls->value);
	if (lease) {
		if (lease->added_av != required_av || lease->users == U32_MAX) {
			ret = -EOVERFLOW;
			goto out_unlock;
		}
		lease->users++;
		state->process_type = lease->process_type;
		state->process_class = lease->process_class;
		state->process_added_av = lease->added_av;
		state->process_lease_refs = 1;
		goto out_unlock;
	}

	direct_av = ksu_direct_allowed_av(db, scontext->type, scontext->type,
					  cls->value);
	add_av = required_av & ~direct_av;
	if (!add_av)
		goto out_unlock;

	new_lease = kzalloc(sizeof(*new_lease), GFP_KERNEL);
	if (!new_lease) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	pol = ksu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("file_load_policy: execmem dup failed: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;
	if (!ksu_apply_process_av(db, src_name, add_av, true)) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}

	state->process_type = scontext->type;
	state->process_class = cls->value;
	state->process_added_av = add_av;
	state->process_lease_refs = 1;

	INIT_LIST_HEAD(&new_lease->list);
	new_lease->process_type = scontext->type;
	new_lease->process_class = cls->value;
	new_lease->added_av = add_av;
	new_lease->users = 1;
	list_add_tail(&new_lease->list, &ksu_process_policy_leases);
	new_lease = NULL;

	pr_info("file_load_policy: allow src=%s process added=0x%x\n", src_name,
		add_av);
	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	ksu_destroy_sepolicy(old_pol);
	reset_avc_cache();

out_unlock:
	kfree(new_lease);
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

int ksu_file_load_policy_allow_execmem_current(
    struct ksu_file_load_policy *state)
{
	return ksu_file_load_policy_allow_execmem_sid(current_sid(), state);
}

int ksu_file_load_policy_allow_execmem_cred(const struct cred *cred,
					    struct ksu_file_load_policy *state)
{
	u32 sid = ksu_file_load_policy_cred_sid(cred);

	if (!sid)
		return -EINVAL;
	return ksu_file_load_policy_allow_execmem_sid(sid, state);
}

int ksu_file_load_policy_restore(const struct ksu_file_load_policy *state)
{
	struct selinux_policy *pol, *old_pol;
	struct ksu_process_policy_lease *lease = NULL;
	struct policydb *db;
	struct ksu_temp_av_key file_key = {};
	struct ksu_temp_av_key tmpfs_key = {};
	struct ksu_temp_av_key dir_key = {};
	const char *src_name;
	const char *tgt_name = NULL;
	const char *tmpfs_name = NULL;
	const char *process_name = NULL;
	u32 file_clear_av = 0;
	u32 tmpfs_clear_av = 0;
	u32 dir_clear_av = 0;
	bool release_process = false;
	bool restore_non_process;
	int ret = 0;

	if (!state)
		return 0;
	if (!!state->added_av != !!state->file_lease_refs ||
	    !!state->tmpfs_added_av != !!state->tmpfs_lease_refs ||
	    !!state->dir_added_av != !!state->dir_lease_refs)
		return -EINVAL;
	if (!!state->process_added_av != !!state->process_lease_refs)
		return -EINVAL;
	if (!state->added_av && !state->tmpfs_added_av &&
	    !state->process_added_av && !state->dir_added_av)
		return 0;

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	db = &old_pol->policydb;
	if (state->added_av) {
		file_key.src_type = state->src_type;
		file_key.tgt_type = state->tgt_type;
		file_key.tclass = state->target_class;
		ret = ksu_temp_av_plan_release_locked(
		    &file_key, state->added_av, state->file_lease_refs,
		    &file_clear_av);
		if (ret)
			goto out_unlock;
	}
	if (state->tmpfs_added_av) {
		tmpfs_key.src_type = state->src_type;
		tmpfs_key.tgt_type = state->tmpfs_type;
		tmpfs_key.tclass = state->target_class;
		ret = ksu_temp_av_plan_release_locked(
		    &tmpfs_key, state->tmpfs_added_av, state->tmpfs_lease_refs,
		    &tmpfs_clear_av);
		if (ret)
			goto out_unlock;
	}
	if (state->dir_added_av) {
		dir_key.src_type = state->src_type;
		dir_key.tgt_type = state->tgt_type;
		dir_key.tclass = state->dir_class;
		ret = ksu_temp_av_plan_release_locked(
		    &dir_key, state->dir_added_av, state->dir_lease_refs,
		    &dir_clear_av);
		if (ret)
			goto out_unlock;
	}
	restore_non_process = file_clear_av || tmpfs_clear_av || dir_clear_av;
	if (state->process_lease_refs) {
		lease = ksu_find_process_policy_lease(state->process_type,
						      state->process_class);
		if (!lease || lease->added_av != state->process_added_av ||
		    state->process_lease_refs > lease->users) {
			ret = -EINVAL;
			goto out_unlock;
		}
		release_process = state->process_lease_refs == lease->users;
		if (!restore_non_process && !release_process) {
			ksu_temp_av_release_locked(&file_key, state->added_av,
						   state->file_lease_refs);
			ksu_temp_av_release_locked(&tmpfs_key,
						   state->tmpfs_added_av,
						   state->tmpfs_lease_refs);
			ksu_temp_av_release_locked(&dir_key,
						   state->dir_added_av,
						   state->dir_lease_refs);
			lease->users -= state->process_lease_refs;
			pr_info(
			    "file_load_policy: process lease retained type=%u "
			    "users=%u\n",
			    lease->process_type, lease->users);
			goto out_unlock;
		}
	}
	if (!restore_non_process && !release_process) {
		ksu_temp_av_release_locked(&file_key, state->added_av,
					   state->file_lease_refs);
		ksu_temp_av_release_locked(&tmpfs_key, state->tmpfs_added_av,
					   state->tmpfs_lease_refs);
		ksu_temp_av_release_locked(&dir_key, state->dir_added_av,
					   state->dir_lease_refs);
		goto out_unlock;
	}

	src_name = restore_non_process
		       ? ksu_type_name_by_value(db, state->src_type)
		       : NULL;
	if (file_clear_av || dir_clear_av)
		tgt_name = ksu_type_name_by_value(db, state->tgt_type);
	if (tmpfs_clear_av)
		tmpfs_name = ksu_type_name_by_value(db, state->tmpfs_type);
	if (release_process)
		process_name = ksu_type_name_by_value(db, state->process_type);
	if ((restore_non_process && !src_name) ||
	    ((file_clear_av || dir_clear_av) && !tgt_name) ||
	    (tmpfs_clear_av && !tmpfs_name) ||
	    (release_process && !process_name)) {
		ret = -ENOENT;
		goto out_unlock;
	}

	pol = ksu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("file_load_policy: restore dup failed: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;
	if (file_clear_av &&
	    !ksu_apply_file_av(db, src_name, tgt_name, file_clear_av, false,
			       ksu_file_load_perms,
			       ARRAY_SIZE(ksu_file_load_perms))) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (tmpfs_clear_av &&
	    !ksu_apply_file_av(db, src_name, tmpfs_name, tmpfs_clear_av, false,
			       ksu_tmpfs_hook_perms,
			       ARRAY_SIZE(ksu_tmpfs_hook_perms))) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (dir_clear_av &&
	    !ksu_apply_dir_av(db, src_name, tgt_name, dir_clear_av, false)) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (release_process &&
	    !ksu_apply_process_av(db, process_name, state->process_added_av,
				  false)) {
		ksu_destroy_sepolicy(pol);
		ret = -EINVAL;
		goto out_unlock;
	}

	pr_info("file_load_policy: restore src=%s tgt=%s file cleared=0x%x "
		"dir=0x%x tmpfs=0x%x process=0x%x refs=%u\n",
		src_name ? src_name : process_name, tgt_name ? tgt_name : "-",
		file_clear_av, dir_clear_av, tmpfs_clear_av,
		release_process ? state->process_added_av : 0,
		state->process_lease_refs);
	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	ksu_destroy_sepolicy(old_pol);
	reset_avc_cache();
	ksu_temp_av_release_locked(&file_key, state->added_av,
				   state->file_lease_refs);
	ksu_temp_av_release_locked(&tmpfs_key, state->tmpfs_added_av,
				   state->tmpfs_lease_refs);
	ksu_temp_av_release_locked(&dir_key, state->dir_added_av,
				   state->dir_lease_refs);
	if (lease) {
		lease->users -= state->process_lease_refs;
		if (!lease->users) {
			list_del(&lease->list);
			kfree(lease);
		}
	}

out_unlock:
	mutex_unlock(&selinux_state.policy_mutex);
	return ret;
}

void apply_kernelsu_rules(void)
{
	struct policydb *db;

	if (!getenforce()) {
		pr_info("SELinux permissive or disabled, apply rules!\n");
	}

	mutex_lock(&ksu_rules);

	backup_original_sepolicy_once();

	db = get_policydb();

	ksu_permissive(db, KERNEL_SU_DOMAIN);
	ksu_typeattribute(db, KERNEL_SU_DOMAIN, "mlstrustedsubject");
	ksu_typeattribute(db, KERNEL_SU_DOMAIN, "netdomain");
	ksu_typeattribute(db, KERNEL_SU_DOMAIN, "bluetoothdomain");

	// Create unconstrained file type
	ksu_type(db, KERNEL_SU_FILE, "file_type");
	ksu_typeattribute(db, KERNEL_SU_FILE, "mlstrustedobject");
	ksu_allow(db, ALL, KERNEL_SU_FILE, ALL, ALL);

	// allow all!
	ksu_allow(db, KERNEL_SU_DOMAIN, ALL, ALL, ALL);

	// allow us do any ioctl
	if (db->policyvers >= POLICYDB_VERSION_XPERMS_IOCTL) {
		ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "blk_file", ALL);
		ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "fifo_file", ALL);
		ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "chr_file", ALL);
		ksu_allowxperm(db, KERNEL_SU_DOMAIN, ALL, "file", ALL);
	}

	// our ksud triggered by init
	ksu_allow(db, "init", KERNEL_SU_DOMAIN, ALL, ALL);

	// copied from Magisk rules
	// suRights
	ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "dir", "search");
	ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "dir", "read");
	ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "file", "open");
	ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "file", "read");
	ksu_allow(db, "servicemanager", KERNEL_SU_DOMAIN, "process", "getattr");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "process", "sigchld");

	// allowLog
	ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "dir", "search");
	ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "read");
	ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "open");
	ksu_allow(db, "logd", KERNEL_SU_DOMAIN, "file", "getattr");

	// dumpsys
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "fd", "use");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "fifo_file", "write");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "fifo_file", "read");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "fifo_file", "open");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "fifo_file", "getattr");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "unix_stream_socket", "read");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "unix_stream_socket", "write");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "unix_stream_socket", "connectto");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "unix_stream_socket", "getopt");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "unix_stream_socket", "getattr");

	// use memfd created by su domain
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "memfd_file", "execute");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "memfd_file", "getattr");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "memfd_file", "map");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "memfd_file", "read");
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "memfd_file", "write");

	// bootctl
	ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "dir", "search");
	ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "file", "read");
	ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "file", "open");
	ksu_allow(db, "hwservicemanager", KERNEL_SU_DOMAIN, "process",
		  "getattr");

	// Allow all binder transactions
	ksu_allow(db, ALL, KERNEL_SU_DOMAIN, "binder", ALL);

	// Allow system server kill su process
	ksu_allow(db, "system_server", KERNEL_SU_DOMAIN, "process", "getpgid");
	ksu_allow(db, "system_server", KERNEL_SU_DOMAIN, "process", "sigkill");

	// YukiZygisk system_server trampolines.
	ksu_allow(db, "system_server", "system_server", "process", "execmem");
	// https://android-review.googlesource.com/c/platform/system/logging/+/3725346
	ksu_dontaudit(db, "untrusted_app", KERNEL_SU_DOMAIN, "dir", "getattr");
	mutex_unlock(&ksu_rules);
}

#define KSU_SEPOLICY_MAX_BATCH_SIZE (8U * 1024U * 1024U)
#define KSU_SEPOLICY_MAX_ARGS 5

struct sepol_data {
	u32 cmd;
	u32 subcmd;
};

struct sepol_batch_cursor {
	const u8 *cur;
	const u8 *end;
};

static size_t sepol_remaining(const struct sepol_batch_cursor *cursor)
{
	return (size_t)(cursor->end - cursor->cur);
}

static int sepol_read_cmd_header(struct sepol_batch_cursor *cursor,
				 struct sepol_data *header)
{
	if (sepol_remaining(cursor) < sizeof(*header))
		return -EINVAL;

	memcpy(header, cursor->cur, sizeof(*header));
	cursor->cur += sizeof(*header);

	return 0;
}

static int sepol_read_string(struct sepol_batch_cursor *cursor,
			     const char **out)
{
	u32 len;
	const char *str;

	if (sepol_remaining(cursor) < sizeof(len))
		return -EINVAL;

	memcpy(&len, cursor->cur, sizeof(len));
	cursor->cur += sizeof(len);

	if (len >= sepol_remaining(cursor))
		return -EINVAL;

	str = (const char *)cursor->cur;
	if (memchr(str, '\0', len) != NULL || str[len] != '\0')
		return -EINVAL;

	cursor->cur += len + 1;
	if (len == 0) {
		*out = ALL;
		return 0;
	}

	*out = str;
	return 0;
}

static int sepol_require_not_all(const char *value, const char *name)
{
	if (value != ALL)
		return 0;

	pr_err("sepol: %s cannot be ALL.\n", name);
	return -EINVAL;
}

static int sepol_expected_argc(u32 cmd)
{
	switch (cmd) {
	case KSU_SEPOLICY_CMD_NORMAL_PERM:
		return 4;
	case KSU_SEPOLICY_CMD_XPERM:
		return 5;
	case KSU_SEPOLICY_CMD_TYPE_STATE:
		return 1;
	case KSU_SEPOLICY_CMD_TYPE:
	case KSU_SEPOLICY_CMD_TYPE_ATTR:
		return 2;
	case KSU_SEPOLICY_CMD_ATTR:
		return 1;
	case KSU_SEPOLICY_CMD_TYPE_TRANSITION:
		return 5;
	case KSU_SEPOLICY_CMD_TYPE_CHANGE:
		return 4;
	case KSU_SEPOLICY_CMD_GENFSCON:
		return 3;
	default:
		return -EINVAL;
	}
}

static int apply_one_sepolicy_cmd(struct policydb *db,
				  const struct sepol_data *header,
				  const char **args)
{
	bool success = false;
	int ret;

	switch (header->cmd) {
	case KSU_SEPOLICY_CMD_NORMAL_PERM:
		if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_ALLOW)
			success =
			    ksu_allow(db, args[0], args[1], args[2], args[3]);
		else if (header->subcmd == KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DENY)
			success =
			    ksu_deny(db, args[0], args[1], args[2], args[3]);
		else if (header->subcmd ==
			 KSU_SEPOLICY_SUBCMD_NORMAL_PERM_AUDITALLOW)
			success = ksu_auditallow(db, args[0], args[1], args[2],
						 args[3]);
		else if (header->subcmd ==
			 KSU_SEPOLICY_SUBCMD_NORMAL_PERM_DONTAUDIT)
			success = ksu_dontaudit(db, args[0], args[1], args[2],
						args[3]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case KSU_SEPOLICY_CMD_XPERM:
		ret = sepol_require_not_all(args[3], "operation");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[4], "perm_set");
		if (ret < 0)
			return ret;

		if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_ALLOW)
			success = ksu_allowxperm(db, args[0], args[1], args[2],
						 args[4]);
		else if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_AUDITALLOW)
			success = ksu_auditallowxperm(db, args[0], args[1],
						      args[2], args[4]);
		else if (header->subcmd == KSU_SEPOLICY_SUBCMD_XPERM_DONTAUDIT)
			success = ksu_dontauditxperm(db, args[0], args[1],
						     args[2], args[4]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case KSU_SEPOLICY_CMD_TYPE_STATE:
		ret = sepol_require_not_all(args[0], "type");
		if (ret < 0)
			return ret;

		if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_STATE_PERMISSIVE)
			success = ksu_permissive(db, args[0]);
		else if (header->subcmd ==
			 KSU_SEPOLICY_SUBCMD_TYPE_STATE_ENFORCE)
			success = ksu_enforce(db, args[0]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case KSU_SEPOLICY_CMD_TYPE:
	case KSU_SEPOLICY_CMD_TYPE_ATTR:
		ret = sepol_require_not_all(args[0], "type");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "attribute");
		if (ret < 0)
			return ret;

		if (header->cmd == KSU_SEPOLICY_CMD_TYPE)
			success = ksu_type(db, args[0], args[1]);
		else
			success = ksu_typeattribute(db, args[0], args[1]);
		if (!success) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	case KSU_SEPOLICY_CMD_ATTR:
		ret = sepol_require_not_all(args[0], "attribute");
		if (ret < 0)
			return ret;

		if (!ksu_attribute(db, args[0])) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	case KSU_SEPOLICY_CMD_TYPE_TRANSITION:
		ret = sepol_require_not_all(args[0], "src");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "tgt");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "cls");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[3], "default_type");
		if (ret < 0)
			return ret;

		success = ksu_type_transition(db, args[0], args[1], args[2],
					      args[3], args[4]);
		return success ? 0 : -EINVAL;

	case KSU_SEPOLICY_CMD_TYPE_CHANGE:
		ret = sepol_require_not_all(args[0], "src");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "tgt");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "cls");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[3], "default_type");
		if (ret < 0)
			return ret;

		if (header->subcmd == KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_CHANGE)
			success = ksu_type_change(db, args[0], args[1], args[2],
						  args[3]);
		else if (header->subcmd ==
			 KSU_SEPOLICY_SUBCMD_TYPE_CHANGE_MEMBER)
			success = ksu_type_member(db, args[0], args[1], args[2],
						  args[3]);
		else
			pr_err("sepol: unknown subcmd: %d\n", header->subcmd);
		return success ? 0 : -EINVAL;

	case KSU_SEPOLICY_CMD_GENFSCON:
		ret = sepol_require_not_all(args[0], "name");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[1], "path");
		if (ret < 0)
			return ret;
		ret = sepol_require_not_all(args[2], "context");
		if (ret < 0)
			return ret;

		if (!ksu_genfscon(db, args[0], args[1], args[2])) {
			pr_err("sepol: %d failed.\n", header->cmd);
			return -EINVAL;
		}
		return 0;

	default:
		pr_err("sepol: unknown cmd: %d\n", header->cmd);
		return -EINVAL;
	}
}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
extern int avc_ss_reset(u32 seqno);
#else
extern int avc_ss_reset(struct selinux_avc *avc, u32 seqno);
#endif // #if (LINUX_VERSION_CODE >= KERNEL_VERSI...
// reset avc cache table, otherwise the new rules will not take effect if
// already denied
static void reset_avc_cache(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0))
	avc_ss_reset(0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(0);
#else
	struct selinux_avc *avc = selinux_state.avc;
	avc_ss_reset(avc, 0);
	selnl_notify_policyload(0);
	selinux_status_update_policyload(&selinux_state, 0);
#endif // #if (LINUX_VERSION_CODE >= KERNEL_VERSI...
	selinux_xfrm_notify_policyload();
}

int handle_sepolicy(void __user *user_data, u64 data_len)
{
	struct selinux_policy *pol, *old_pol;
	struct policydb *db;
	struct sepol_batch_cursor cursor;
	u8 *payload;
	int ret;
	int success_cmd_count;
	u32 cmd_index;

	if (!user_data || !data_len)
		return -EINVAL;

	if (data_len > KSU_SEPOLICY_MAX_BATCH_SIZE)
		return -E2BIG;

	payload = kvmalloc((size_t)data_len, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	if (copy_from_user(payload, user_data, (size_t)data_len)) {
		ret = -EFAULT;
		goto out_free;
	}

	if (!getenforce()) {
		pr_info("SELinux permissive or disabled when handle policy!\n");
	}

	mutex_lock(&selinux_state.policy_mutex);

	old_pol = selinux_state.policy;
	pol = ksu_dup_sepolicy(rcu_dereference_protected(
	    old_pol, lockdep_is_held(&selinux_state.policy_mutex)));
	if (IS_ERR(pol)) {
		ret = PTR_ERR(pol);
		pr_err("ksu_dup_sepolicy err: %d\n", ret);
		goto out_unlock;
	}
	db = &pol->policydb;

	cursor.cur = payload;
	cursor.end = payload + (size_t)data_len;

	ret = 0;
	success_cmd_count = 0;
	cmd_index = 0;
	while (cursor.cur < cursor.end) {
		struct sepol_data header;
		const char *args[KSU_SEPOLICY_MAX_ARGS] = {0};
		int expected_argc;
		u32 arg_index;

		ret = sepol_read_cmd_header(&cursor, &header);
		if (ret < 0) {
			pr_err("sepol: failed to read cmd header #%u.\n",
			       cmd_index);
			goto out_drop_new_policy;
		}

		expected_argc = sepol_expected_argc(header.cmd);
		if (expected_argc < 0 ||
		    expected_argc > KSU_SEPOLICY_MAX_ARGS) {
			ret = -EINVAL;
			pr_err("sepol: invalid cmd header #%u.\n", cmd_index);
			goto out_drop_new_policy;
		}

		for (arg_index = 0; arg_index < (u32)expected_argc;
		     arg_index++) {
			ret = sepol_read_string(&cursor, &args[arg_index]);
			if (ret < 0) {
				pr_err("sepol: failed to read cmd #%u arg "
				       "#%u.\n",
				       cmd_index, arg_index);
				goto out_drop_new_policy;
			}
		}

		ret = apply_one_sepolicy_cmd(db, &header, args);
		if (ret < 0) {
			pr_err("sepol: cmd #%u failed, cmd=%u subcmd=%u.\n",
			       cmd_index, header.cmd, header.subcmd);
		} else {
			success_cmd_count++;
		}
		cmd_index++;
	}

	rcu_assign_pointer(selinux_state.policy, pol);
	synchronize_rcu();
	ksu_destroy_sepolicy(old_pol);

	reset_avc_cache();
	ret = success_cmd_count;
	goto out_unlock;

out_drop_new_policy:
	ksu_destroy_sepolicy(pol);
out_unlock:
	mutex_unlock(&selinux_state.policy_mutex);
out_free:
	kvfree(payload);
	return ret;
}
