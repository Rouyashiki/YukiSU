#ifndef _KASUMI_OVERLAY_H
#define _KASUMI_OVERLAY_H

#include <linux/fs.h>
#include <linux/list.h>

void kasumi_add_inject_rule(char *dir);
void kasumi_prune_inject_parent(const char *deleted_path);
int kasumi_check_merge_target(const char *target);
int kasumi_materialize_merge(const char *src_prefix, const char *target_dir,
			     int depth, u32 rule_flags);
bool kasumi_merge_child_visible(struct inode *parent, const char *dir,
				const char *name, int len);
void kasumi_populate_injected_list(const char *dir_path, struct dentry *parent,
				   struct list_head *head);

#endif /* _KASUMI_OVERLAY_H */
