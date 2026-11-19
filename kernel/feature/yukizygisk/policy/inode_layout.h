#if defined(CONFIG_QUEUED_SPINLOCKS) && !defined(CONFIG_PREEMPT_RT) &&         \
    !defined(CONFIG_DEBUG_SPINLOCK) && !defined(CONFIG_DEBUG_LOCK_ALLOC)
struct yz_inode_btf {
	const struct btf *btf;
	typeof(&btf_name_by_offset) name;
	typeof(&btf_type_skip_modifiers) resolve;
};

/* Descend through the anonymous unions in spinlock_t and arch_spinlock_t. */
static YZ_BTF_CALL const struct btf_type *
yz_btf_member(const struct yz_inode_btf *ops, const struct btf_type *parent,
	      const char *name, u32 offset, u32 size, unsigned int depth)
{
	const struct btf_member *m;
	u32 i;

	if (!parent ||
	    (BTF_INFO_KIND(parent->info) != BTF_KIND_STRUCT &&
	     BTF_INFO_KIND(parent->info) != BTF_KIND_UNION) ||
	    offset > parent->size || size > parent->size - offset)
		return NULL;
	for_each_member(i, parent, m)
	{
		const struct btf_type *type;
		const char *field;
		u32 bits = m->offset;

		if (BTF_INFO_KFLAG(parent->info)) {
			if (BTF_MEMBER_BITFIELD_SIZE(bits))
				continue;
			bits = BTF_MEMBER_BIT_OFFSET(bits);
		}
		if ((bits & 7) || bits / 8 > offset)
			continue;
		field = ops->name(ops->btf, m->name_off);
		if (!field || (*field && strcmp(field, name)))
			continue;
		type = ops->resolve(ops->btf, m->type, NULL);
		if (!type)
			continue;
		if (*field) {
			if (bits / 8 == offset && type->size == size)
				return type;
		} else if (depth) {
			type = yz_btf_member(ops, type, name, offset - bits / 8,
					     size, depth - 1);
			if (type)
				return type;
		}
	}
	return NULL;
}

static YZ_BTF_CALL bool yz_btf_struct(const struct yz_inode_btf *ops,
				      const struct btf_type *type,
				      const char *expected)
{
	const char *name;

	if (!type || BTF_INFO_KIND(type->info) != BTF_KIND_STRUCT)
		return false;
	name = ops->name(ops->btf, type->name_off);
	return name && !strcmp(name, expected);
}

static bool yz_btf_integer(const struct btf_type *type, u32 size)
{
	u32 encoding;

	if (!type || BTF_INFO_KIND(type->info) != BTF_KIND_INT)
		return false;
	encoding = *(const u32 *)(type + 1);
	return !BTF_INT_OFFSET(encoding) &&
	       BTF_INT_BITS(encoding) == size * 8 &&
	       !(BTF_INT_ENCODING(encoding) & BTF_INT_BOOL);
}

#define YZ_MEMBER(parent, structure, field)                                    \
	yz_btf_member(&ops, parent, #field, offsetof(structure, field),        \
		      sizeof_field(structure, field), 2)
#define YZ_INTEGER(parent, structure, field)                                   \
	yz_btf_integer(YZ_MEMBER(parent, structure, field),                    \
		       sizeof_field(structure, field))

static bool yz_fixed_inode_btf(const struct btf *btf,
			       const struct btf_type *inode,
			       typeof(&btf_name_by_offset) name,
			       typeof(&btf_type_skip_modifiers) resolve)
{
	const struct yz_inode_btf ops = {btf, name, resolve};
	const struct btf_type *lock =
	    YZ_MEMBER(inode, struct inode_security_struct, lock);
	const struct btf_type *raw = YZ_MEMBER(lock, spinlock_t, rlock);
	const struct btf_type *arch = YZ_MEMBER(raw, raw_spinlock_t, raw_lock);
	const struct btf_type *val = YZ_MEMBER(arch, arch_spinlock_t, val);

	return yz_btf_struct(&ops, lock, "spinlock") &&
	       yz_btf_struct(&ops, raw, "raw_spinlock") &&
	       yz_btf_struct(&ops, arch, "qspinlock") &&
	       YZ_INTEGER(inode, struct inode_security_struct, task_sid) &&
	       YZ_INTEGER(inode, struct inode_security_struct, sclass) &&
	       YZ_INTEGER(inode, struct inode_security_struct, initialized) &&
	       YZ_INTEGER(val, atomic_t, counter) &&
	       YZ_INTEGER(arch, arch_spinlock_t, locked) &&
	       YZ_INTEGER(arch, arch_spinlock_t, pending) &&
	       YZ_INTEGER(arch, arch_spinlock_t, locked_pending) &&
	       YZ_INTEGER(arch, arch_spinlock_t, tail);
}
#undef YZ_INTEGER
#undef YZ_MEMBER
#else
static bool yz_fixed_inode_btf(const struct btf *btf,
			       const struct btf_type *inode,
			       typeof(&btf_name_by_offset) name,
			       typeof(&btf_type_skip_modifiers) resolve)
{
	return false;
}
#endif
