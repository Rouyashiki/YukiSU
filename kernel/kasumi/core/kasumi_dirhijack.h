#ifndef _KASUMI_DIRHIJACK_H
#define _KASUMI_DIRHIJACK_H

#include <linux/path.h>
#include <linux/types.h>

struct inode;
struct file;
struct list_head;
struct inode_operations;
struct super_operations;

int kasumi_dirhijack_init(void);
void kasumi_dirhijack_stop_new(void);
void kasumi_dirhijack_exit(void);

bool kasumi_dirhijack_enabled(void);
const struct inode_operations *
kasumi_dirhijack_original_iops(const struct inode *inode);
/* Sleepable lower-getattr override update. Hold no spinlock or DH SRCU read
 * section. The caller serializes lower-owner updates and retains both vectors
 * until replacement, the lower owner's callback drain and this SRCU drain end.
 */
int kasumi_dirhijack_replace_iops(struct inode *inode,
	const struct inode_operations *expected,
	const struct inode_operations *replacement);
void kasumi_dirhijack_synchronize_iops(void);

/*
 * Virtual-inode reclaim leaf functions, invoked by the unified super_operations
 * owner (kasumi_sop_shadow) from its destroy/evict/drop_inode trampolines with
 * @orig already resolved under RCU.  The trampoline owns the active-counter
 * drain, so these bodies may early-return freely.  Kept here so the
 * load-bearing kasumi_vnode reclaim branches stay co-located with the rest of
 * dirhijack.
 */
void kasumi_dh_reclaim_evict_inode(struct inode *inode,
				   const struct super_operations *orig);
int kasumi_dh_reclaim_drop_inode(struct inode *inode,
				 const struct super_operations *orig);

/*
 * Register a virtual child at @visible_path backed by @source, projected with
 * stable identity @v_ino.  Resolves the visible parent directory, installs the
 * lookup/superblock hijack there if needed, and indexes the child.  Sleepable
 * context only (control path).  Returns 0 or a negative errno.
 */
int kasumi_dirhijack_add(const char *visible_path, const struct path *source,
			 unsigned long v_ino, u8 flags);

int kasumi_dirhijack_add_su(const char *visible_path, unsigned long v_ino,
			    struct path *parent);
/* Return 1 for an unchanged binding, 0 for a mismatch, or a negative errno. */
int kasumi_dirhijack_match_su(const char *visible_path,
			      const struct path *parent, unsigned long v_ino);
void kasumi_dirhijack_del_su(const struct path *parent, const char *name,
			     unsigned long v_ino);

/*
 * Register a lookup-only child at @visible_path backed by @source: VFS lookup
 * resolves it to a Kasumi vnode, but dirhijack does not shadow this dir's
 * readdir — the overlay filldir injection keeps emitting and deduping the name.
 * Sinks a merge-materialized file's lookup axis onto the VFS layer (Slice 4a).
 * Sleepable context only.  Returns 0 or a negative errno.
 */
int kasumi_dirhijack_add_shadow(const char *visible_path,
				const struct path *source, unsigned long v_ino,
				u8 flags);

/* The caller holds kasumi_mutation_mutex from begin through finish. */
struct kasumi_dirhijack_batch;
struct kasumi_dirhijack_batch *kasumi_dirhijack_batch_begin(u64 generation);
int kasumi_dirhijack_batch_add(struct kasumi_dirhijack_batch *batch,
	const char *visible_path, const struct path *source,
	unsigned long ino, u8 flags, bool lookup_only);
int kasumi_dirhijack_batch_hide(struct kasumi_dirhijack_batch *batch,
	const char *visible_path, u32 rule_flags);
int kasumi_dirhijack_batch_del(struct kasumi_dirhijack_batch *batch,
	const char *visible_path);
void kasumi_dirhijack_batch_finish(struct kasumi_dirhijack_batch *batch,
	bool published);

/* Remove one registered child (by visible path). */
int kasumi_dirhijack_del(const char *visible_path);

/*
 * Register a suppress-only ("hide") child at @visible_path: VFS lookup returns
 * a negative dentry and readdir omits the name for hide-target observers.
 * Ordinary rules exempt root; internal rule-management lookups always retain
 * the real entry. Sleepable context only.
 */
int kasumi_dirhijack_hide(const char *visible_path, u32 rule_flags);

struct kasumi_hide_binding;
int kasumi_dirhijack_hide_get(const struct path *parent, const char *name,
			      u32 flags, struct kasumi_hide_binding **result);
void kasumi_dirhijack_hide_put(struct kasumi_hide_binding *binding);
bool kasumi_dirhijack_hide_matches(const struct kasumi_hide_binding *binding,
				   const struct path *parent, u32 flags);
/* Return 1 for an injected child, 0 for a real/hidden view, -ENOENT if unbound.
 */
int kasumi_dirhijack_child_visible(struct inode *parent, const char *name,
				   int len);

bool kasumi_dirhijack_hidden(struct inode *parent, const char *name, int len);
int kasumi_dirhijack_collect(struct file *file, struct list_head *entries,
			     u64 *generation, void *data);

/*
 * Restore every inode/file/dentry operation shadow, invalidate affected
 * dentries, drain callbacks, and release all dirhijack metadata.  May sleep.
 */
void kasumi_dirhijack_clear(void);

#endif /* _KASUMI_DIRHIJACK_H */
