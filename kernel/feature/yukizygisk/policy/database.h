/* The including host supplies symbol calls and the AV insertion adapter. */
struct yz_fixed_rule {
	const char *source;
	const char *target;
	const char *class_name;
	const char *permission;
};

static const struct yz_fixed_rule yz_fixed_rules[] = {
#include "policy/rules.h"
};

static int yz_fixed_add_attribute(struct policydb *db, u32 type,
				  const char *name)
{
	struct type_datum *attr = yz_fp_search(&db->p_types, name);
	u32 i;
	int ret;

	if (!attr || !attr->attribute)
		return -ENOENT;
	ret = yz_fp_set_bit(&db->type_attr_map_array[type - 1], attr->value - 1,
			    1);
	if (ret)
		return ret;
	db->len += 12;
	for (i = 0; i < db->p_classes.nprim; i++) {
		struct constraint_node *node;

		for (node = db->class_val_to_struct[i]->constraints; node;
		     node = node->next) {
			struct constraint_expr *expression;

			for (expression = node->expr; expression;
			     expression = expression->next) {
				if (expression->expr_type != CEXPR_NAMES ||
				    !expression->type_names ||
				    !ksu_ebitmap_has_bit(
					&expression->type_names->types,
					attr->value - 1))
					continue;
				ret = yz_fp_set_bit(&expression->names,
						    type - 1, 1);
				if (ret)
					return ret;
				db->len += 12;
			}
		}
	}
	return 0;
}

static int yz_fixed_verify_file_type(struct policydb *db)
{
	struct type_datum *type =
	    yz_fp_search(&db->p_types, YZ_FIXED_FILE_TYPE);
	struct type_datum *file_attr = yz_fp_search(&db->p_types, "file_type");
	struct type_datum *trusted =
	    yz_fp_search(&db->p_types, "mlstrustedobject");
	struct type_datum *domain = yz_fp_search(&db->p_types, "domain");
	struct ebitmap_node *node;
	struct ebitmap *map;
	u32 bit, members = 0;

	if (!file_attr || !file_attr->attribute || !trusted ||
	    !trusted->attribute || !domain || !domain->attribute ||
	    !domain->value || domain->value > db->p_types.nprim ||
	    !db->sym_val_to_name[SYM_TYPES][domain->value - 1] ||
	    strcmp(db->sym_val_to_name[SYM_TYPES][domain->value - 1], "domain"))
		return -EINVAL;
	if (!type)
		return -ENOENT;
	if (type->attribute || !type->primary || !type->value ||
	    type->value > db->p_types.nprim ||
	    !db->sym_val_to_name[SYM_TYPES][type->value - 1] ||
	    strcmp(db->sym_val_to_name[SYM_TYPES][type->value - 1],
		   YZ_FIXED_FILE_TYPE))
		return -EEXIST;

	/* An additional attribute can grant access through unrelated rules. */
	map = &db->type_attr_map_array[type->value - 1];
	ebitmap_for_each_positive_bit(map, node, bit)
	{
		if (bit != type->value - 1 && bit != file_attr->value - 1 &&
		    bit != trusted->value - 1)
			return -EEXIST;
		members++;
	}
	return members == 3 ? 0 : -EEXIST;
}

static int yz_fixed_add_file_type(struct policydb *db)
{
	struct type_datum *type;
	struct type_datum *attr;
	struct ebitmap *maps;
	struct type_datum **types;
	char **names;
	char *name;
	u32 count = db->p_types.nprim;
	u32 i;
	int ret;

	ret = yz_fixed_verify_file_type(db);
	if (ret != -ENOENT)
		return ret;
	attr = yz_fp_search(&db->p_types, "file_type");
	if (!attr || !attr->attribute || count >= U16_MAX)
		return -EINVAL;
	type = kzalloc(sizeof(*type), GFP_KERNEL);
	name = kstrdup(YZ_FIXED_FILE_TYPE, GFP_KERNEL);
	maps = kvcalloc(count + 1, sizeof(*maps), GFP_KERNEL);
	types = kvcalloc(count + 1, sizeof(*types), GFP_KERNEL);
	names = kvcalloc(count + 1, sizeof(*names), GFP_KERNEL);
	if (!type || !name || !maps || !types || !names) {
		ret = -ENOMEM;
		goto fail;
	}
	type->primary = 1;
	type->value = count + 1;
	ret = yz_fp_set_bit(&maps[count], count, 1);
	if (ret)
		goto fail_map;
	ret = yz_fp_set_bit(&maps[count], attr->value - 1, 1);
	if (ret)
		goto fail_map;
	ret = yz_fp_insert(&db->p_types, name, type);
	if (ret)
		goto fail_map;
	memcpy(maps, db->type_attr_map_array, count * sizeof(*maps));
	memcpy(types, db->type_val_to_struct, count * sizeof(*types));
	memcpy(names, db->sym_val_to_name[SYM_TYPES], count * sizeof(*names));
	types[count] = type;
	names[count] = name;
	kvfree(db->type_attr_map_array);
	kvfree(db->type_val_to_struct);
	kvfree(db->sym_val_to_name[SYM_TYPES]);
	db->type_attr_map_array = maps;
	db->type_val_to_struct = types;
	db->sym_val_to_name[SYM_TYPES] = names;
	db->p_types.nprim++;
	/* Bound the extra serialized symbol, maps and role bitmaps. */
	db->len += 256 + db->p_roles.nprim * 12;
	for (i = 0; i < db->p_roles.nprim; i++) {
		ret =
		    yz_fp_set_bit(&db->role_val_to_struct[i]->types, count, 1);
		if (ret)
			return ret;
	}
	ret = yz_fixed_add_attribute(db, count + 1, "file_type");
	if (ret)
		return ret;
	return yz_fixed_add_attribute(db, count + 1, "mlstrustedobject");
fail_map:
	yz_fp_destroy_bitmap(&maps[count]);
fail:
	kvfree(maps);
	kvfree(types);
	kvfree(names);
	kfree(type);
	kfree(name);
	return ret;
}

static const char *yz_fixed_resolve(const char *name, const char *daemon)
{
	if (!strcmp(name, "$daemon"))
		return daemon;
	return name;
}

static int yz_fixed_apply_rules(struct policydb *db,
				const struct yz_fixed_policy_config *config,
				const char *daemon, bool install,
				struct yz_fixed_policy_status *detail)
{
	struct type_datum *daemon_type;
	size_t j;
	int ret;

	(void)config;
	if (!install && !yz_fp_search(&db->p_types, YZ_FIXED_FILE_TYPE))
		return -ENOENT;
	ret = yz_fixed_add_file_type(db);
	if (ret)
		return ret;
	daemon_type = yz_fp_search(&db->p_types, daemon);
	if (!daemon_type || !daemon_type->primary || daemon_type->attribute ||
	    !daemon_type->value || daemon_type->value > db->p_types.nprim ||
	    !db->sym_val_to_name[SYM_TYPES][daemon_type->value - 1] ||
	    strcmp(db->sym_val_to_name[SYM_TYPES][daemon_type->value - 1],
		   daemon))
		return -EINVAL;
	for (j = 0; j < ARRAY_SIZE(yz_fixed_rules); j++) {
		const struct yz_fixed_rule *rule = &yz_fixed_rules[j];
		const char *source = yz_fixed_resolve(rule->source, daemon);
		const char *dest = yz_fixed_resolve(rule->target, daemon);

		/* Only the class absent from older policies is optional. */
		if (!strcmp(rule->class_name, "memfd_file") &&
		    !yz_fp_search(&db->p_classes, rule->class_name))
			continue;
		ret = (install ? yz_fp_allow : yz_fp_has_rule)(
		    db, source, dest, rule->class_name, rule->permission);
		if (!ret)
			continue;
		if (detail) {
			struct class_datum *cls =
			    yz_fp_search(&db->p_classes, rule->class_name);
			u32 av = ksu_file_perm_mask(cls, rule->permission);

			yz_fixed_set_missing(detail, source, dest,
					     rule->class_name, av);
		}
		if (install)
			pr_err("yukizygisk: fixed rule failed %s %s %s %s "
			       "error=%d\n",
			       source, dest, rule->class_name, rule->permission,
			       ret);
		return ret;
	}
	return 0;
}
