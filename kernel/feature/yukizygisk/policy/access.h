#include <linux/shmem_fs.h>

static struct selinux_policy *yz_fixed_lock_policy(void)
{
	struct selinux_policy *policy;
	int ret;

	mutex_lock(&yz_fp_state->policy_mutex);
	policy = rcu_dereference_protected(
	    yz_fp_state->policy, lockdep_is_held(&yz_fp_state->policy_mutex));
	ret = yz_fixed_live_error(policy, NULL);
	if (ret) {
		mutex_unlock(&yz_fp_state->policy_mutex);
		return ERR_PTR(ret);
	}
	return policy;
}

static const char *yz_fixed_type_name(const struct policydb *db,
				      const struct context *ctx)
{
	const void *names = db->sym_val_to_name[SYM_TYPES];
	const char *name;

	if (!ctx || !ctx->type || ctx->type > db->p_types.nprim || !names)
		return "?";
	name = db->sym_val_to_name[SYM_TYPES][ctx->type - 1];
	return name ?: "?";
}

static int yz_fixed_check_access_locked(struct selinux_policy *policy,
					u32 source, u32 target,
					const char *class_name,
					const char *const *permissions,
					size_t count)
{
	struct class_datum *cls;
	struct av_decision decision;
	u32 required = 0;
	bool enforcing = true;
	size_t i;

	lockdep_assert_held(&yz_fp_state->policy_mutex);
	if (!source || !target)
		return -EINVAL;
	cls = yz_fp_search(&policy->policydb.p_classes, class_name);
	if (!cls)
		return -ENOENT;
	for (i = 0; i < count; i++) {
		struct perm_datum *perm =
		    yz_fp_search(&cls->permissions, permissions[i]);

		if (!perm && cls->comdatum)
			perm = yz_fp_search(&cls->comdatum->permissions,
					    permissions[i]);
		if (!perm || !perm->value || perm->value > 32)
			return -ENOENT;
		required |= 1U << (perm->value - 1);
	}
	yz_fp_compute(source, target, cls->value, &decision);
#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	enforcing = READ_ONCE(yz_fp_state->enforcing);
#endif
	if (enforcing && !(decision.flags & AVD_FLAGS_PERMISSIVE) &&
	    (decision.allowed & required) != required) {
		struct context *sctx =
		    yz_fp_sidtab_search(policy->sidtab, source);
		struct context *tctx =
		    yz_fp_sidtab_search(policy->sidtab, target);
		const char *sname = yz_fixed_type_name(&policy->policydb, sctx);
		const char *tname = yz_fixed_type_name(&policy->policydb, tctx);

		yz_fixed_policy_missing(sname, tname, class_name,
					required & ~decision.allowed);
		pr_warn_ratelimited(
		    "yukizygisk: fixed policy missing src=%s tgt=%s class=%s "
		    "av=0x%x\n",
		    sname, tname, class_name, required & ~decision.allowed);
		return -EACCES;
	}
	return 0;
}

int yz_fixed_policy_check_execmem(const struct cred *cred)
{
	static const char *const perms[] = {"execmem"};
	u32 sid = yz_fp_cred_sid(cred);
	struct selinux_policy *policy = yz_fixed_lock_policy();
	int ret;

	if (IS_ERR(policy))
		return PTR_ERR(policy);
	ret = yz_fixed_check_access_locked(policy, sid, sid, "process", perms,
					   ARRAY_SIZE(perms));
	mutex_unlock(&yz_fp_state->policy_mutex);
	return ret;
}

static int yz_fixed_check_file_locked(struct selinux_policy *policy,
				      struct file *file,
				      const struct cred *cred, bool check_exec,
				      bool check_fd, u32 target_sid)
{
	static const char *const directory[] = {"read", "open", "getattr",
						"search"};
	static const char *const image[] = {"read", "open", "getattr", "map",
					    "execute"};
	static const char *const receive[] = {"read", "open", "getattr", "map"};
	static const char *const fd_use[] = {"use"};
	struct inode_security_struct *isec;
	const char *class_name;
	const char *const *perms;
	size_t count;
	u32 source;
	int ret;

	if (!file || !cred)
		return -EINVAL;
	isec = yz_fp_inode(file_inode(file));
	if (IS_ERR_OR_NULL(isec))
		return isec ? PTR_ERR(isec) : -EINVAL;
	source = yz_fp_cred_sid(cred);
	if (S_ISDIR(file_inode(file)->i_mode)) {
		class_name = "dir";
		perms = directory;
		count = ARRAY_SIZE(directory);
	} else {
		class_name = "file";
#ifdef SECCLASS_MEMFD_FILE
		if (isec->sclass == SECCLASS_MEMFD_FILE)
			class_name = "memfd_file";
#endif
		perms = check_exec ? image : receive;
		count = check_exec ? ARRAY_SIZE(image) : ARRAY_SIZE(receive);
	}
	ret = yz_fixed_check_access_locked(
	    policy, source, target_sid ?: isec->sid, class_name, perms, count);
	if (!ret && check_fd)
		ret = yz_fixed_check_access_locked(
		    policy, source, yz_fp_cred_sid(file->f_cred), "fd", fd_use,
		    ARRAY_SIZE(fd_use));
	return ret;
}

int yz_fixed_policy_check(struct file *file, const struct cred *cred,
			  bool receive_only)
{
	struct selinux_policy *policy = yz_fixed_lock_policy();
	int ret;

	if (IS_ERR(policy))
		return PTR_ERR(policy);
	ret = yz_fixed_check_file_locked(policy, file, cred, !receive_only,
					 receive_only, 0);
	mutex_unlock(&yz_fp_state->policy_mutex);
	return ret;
}

static bool
yz_fixed_private_module_image(struct file *file,
			      const struct inode_security_struct *isec)
{
	const unsigned int required =
	    F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
	struct inode *inode = file_inode(file);
	u32 daemon_sid = yz_fp_cred_sid(current_cred());

	/* memfd_create uses an unlinked pseudo dentry on the internal shmem
	 * mount. */
	return (file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ &&
	       (inode->i_sb->s_flags & SB_KERNMOUNT) &&
	       IS_ROOT(file->f_path.dentry) && !inode->i_nlink &&
	       !IS_PRIVATE(inode) && i_size_read(inode) > 0 &&
	       uid_eq(inode->i_uid, current_fsuid()) &&
	       uid_eq(file->f_cred->fsuid, current_fsuid()) && daemon_sid &&
	       isec->task_sid == daemon_sid &&
	       yz_fp_cred_sid(file->f_cred) == daemon_sid &&
	       (READ_ONCE(SHMEM_I(inode)->seals) & required) == required;
}

int yz_fixed_policy_label(struct file *file, const struct cred *receiver)
{
	struct inode_security_struct *isec;
	struct selinux_policy *policy;
	u32 sid;
	int ret;

	if (!yz_fixed_policy_active())
		return 0;
	if (!file || file_inode(file)->i_sb->s_magic != TMPFS_MAGIC ||
	    !S_ISREG(file_inode(file)->i_mode))
		return -EINVAL;
	isec = yz_fp_inode(file_inode(file));
	if (IS_ERR_OR_NULL(isec))
		return isec ? PTR_ERR(isec) : -EINVAL;
	if (receiver && !yz_fixed_private_module_image(file, isec))
		return -EPERM;
	/* Keep SID resolution, both receive checks and publication in one
	 * generation. */
	policy = yz_fixed_lock_policy();
	if (IS_ERR(policy))
		return PTR_ERR(policy);
	ret = yz_fp_context_sid(YZ_FIXED_FILE_CONTEXT,
				sizeof(YZ_FIXED_FILE_CONTEXT) - 1, &sid,
				GFP_KERNEL);
	if (ret || !sid) {
		ret = ret ?: -EINVAL;
		goto unlock;
	}
	if (receiver) {
		ret = yz_fixed_check_file_locked(policy, file, receiver, true,
						 true, sid);
		if (ret)
			goto unlock;
	}
	spin_lock(&isec->lock);
	if (!isec->sclass)
		isec->sclass = SECCLASS_FILE;
	isec->sid = sid;
	isec->initialized = LABEL_INITIALIZED;
	spin_unlock(&isec->lock);
unlock:
	mutex_unlock(&yz_fp_state->policy_mutex);
	return ret;
}
