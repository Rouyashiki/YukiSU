#ifndef YZ_POLICYDB_CLEANUP_H
#define YZ_POLICYDB_CLEANUP_H

#include "ss/policydb.h"

static inline bool policydb_read_needs_destroy(const struct policydb *p)
{
	u32 i;

	/* The symtab/roles init exits retain only these allocation roots.
	 * policydb_destroy() clears them on every already-cleaned error path.
	 */
	for (i = 0; i < SYM_NUM; i++) {
		if (p->symtab[i].table.htable)
			return true;
	}
	return p->policycaps.node || p->permissive_map.node;
}

#endif
