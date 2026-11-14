#ifndef _KASUMI_PATH_POLICY_H
#define _KASUMI_PATH_POLICY_H

#include <linux/list.h>
#include <linux/errno.h>
#include <linux/path.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <uapi/linux/magic.h>

#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif

struct kasumi_entry;
struct kasumi_rule_lifetime;
void kasumi_rule_index_add(struct kasumi_rule_lifetime *life);
void kasumi_rule_index_remove(struct kasumi_rule_lifetime *life);
unsigned long kasumi_rule_source_ino(dev_t dev, u64 ino);
/* The callback runs under RCU and the index read lock. It cannot sleep or
 * mutate rule stores. Retain RCU in the caller when keeping entry pointers. */
void kasumi_rule_dependencies(const char *path, u64 generation,
	void (*visit)(struct kasumi_rule_lifetime *, void *), void *data);
struct kasumi_inode_identity;
struct qstr;
struct inode;
struct file;

struct kasumi_rule_source {
	struct path path;
	struct kasumi_inode_identity *identity;
	struct kstat stat;
	umode_t source_mode;
	unsigned long visible_ino;
	unsigned long visible_dev;
	bool stat_valid;
	bool preserve_visible_metadata;
	int error;
};

void kasumi_project_visible_stat(struct kstat *result,
				 const struct kstat *source_stat,
				 const struct kstat *visible_template,
				 bool preserve_visible_metadata,
				 unsigned long visible_ino,
				 unsigned long visible_dev);

enum kasumi_policy_scope {
	KASUMI_POLICY_SCOPE_NONE = 0,
	KASUMI_POLICY_SCOPE_VIEW,
	KASUMI_POLICY_SCOPE_SPOOF,
};

bool kasumi_is_privileged_process(void);
bool kasumi_policy_current_is_isolated(void);
enum kasumi_policy_scope kasumi_policy_current_scope(void);
bool kasumi_policy_current_is_view_target(void);
bool kasumi_rule_scope_active(void);
bool kasumi_rule_current_sees(u32 flags);
bool kasumi_rule_current_hides(const struct inode *parent, u32 flags);
/* Management-only, under mutation_mutex. Returns 1 for vnode parents, 0 for
 * native parents, or an error. A nonempty source_parent owns a path reference.
 */
int kasumi_rule_projected_parent(const char *visible, u64 generation,
				 struct path *source_parent);
bool kasumi_rule_vpath_visible(const char *path);
bool kasumi_hide_storage_parent(const struct inode *parent);
bool kasumi_hide_scope_allowed(bool storage_managed, u32 flags);
bool kasumi_policy_current_is_hide_target(const struct inode *parent);
bool kasumi_policy_current_is_spoof_target(void);
bool kasumi_policy_uid_is_spoof_target(uid_t uid);
bool kasumi_rule_get_source_flags(const char *pathname,
				  unsigned int lookup_flags,
				  struct kasumi_rule_source *source);
void kasumi_rule_source_put(struct kasumi_rule_source *source);
bool kasumi_should_hide(const char *pathname);
int kasumi_rule_child_mutation(struct inode *parent, const char *visible_path,
			       const struct qstr *name);

/*
 * Slice 4c-v2 pure-virtual directory topology: resolve names under a synthetic
 * (source-less) directory against the rule table.  A F_VIRTUAL_DIR vnode uses
 * these to decide whether a child name is an exact redirect (a leaf) or the
 * prefix of one (a deeper virtual dir), and to enumerate its children.
 */
enum kasumi_vpath_kind {
	KASUMI_VPATH_HIDDEN = -ENOENT,
	KASUMI_VPATH_NONE = 0, /* no rule under dir/child */
	KASUMI_VPATH_LEAF, /* dir/child is an exact rule -> a real redirect */
	KASUMI_VPATH_VDIR, /* dir/child is a prefix of some rule -> virtual dir
			    */
};

/*
 * Resolve @child within virtual directory @dir.  On KASUMI_VPATH_LEAF pins
 * *leaf_src (caller path_put) and fills *leaf_mode / *leaf_ino with the
 * leaf's source identity (the nofollow link for a symlink target). Non-sleeping
 * apart from a GFP_KERNEL scratch alloc; the rule-table scan runs under RCU.
 * Returns -ENOMEM if the scratch allocation fails.
 */
int kasumi_rule_vpath_child(const char *dir, const char *child,
			    struct path *leaf_src, umode_t *leaf_mode,
			    unsigned long *leaf_ino,
			    struct kasumi_inode_identity **identity,
			    struct dentry *parent);

bool kasumi_rule_directory_has_injects(const char *path, u64 generation);
bool kasumi_rule_directory_overridden(const char *path, u64 generation);
bool kasumi_rule_vpath_flags(const char *path, u64 generation, u32 *flags);
int kasumi_rule_vpath_collect(struct file *file, struct list_head *out,
			     u64 *generation, void *data);

#endif /* _KASUMI_PATH_POLICY_H */
