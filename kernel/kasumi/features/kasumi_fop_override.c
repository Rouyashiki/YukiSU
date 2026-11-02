#include "kasumi_entrypoints.h"
#include "kasumi_fop_bridge.h"
#include "kasumi_fop_override.h"
#include "kasumi_runtime.h"
#include "kasumi_vfs_hooks.h"

#include <linux/hashtable.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>

#define KASUMI_FOP_HASH_BITS 10
#define KASUMI_FOP_TEMPLATE_HASH_BITS 6

struct kasumi_fop_template {
	const struct file_operations *orig_fop;
	struct file_operations ingress_fop;
	struct file_operations shadow_fop;
	struct kasumi_fop_bridge_entry bridge;
	struct hlist_node node;
	struct list_head retire_node;
};

struct kasumi_fop_meta {
	struct inode *inode;
	struct super_block *sb;
	unsigned int inject_owners;
	unsigned int iterate_owners;
	bool retired;
	struct kasumi_fop_template *template;
	kasumi_fop_iterate_client_fn iterate_client;
	void *iterate_client_data;
	struct hlist_node node;
	struct list_head retire_node;
};

static DEFINE_HASHTABLE(kasumi_fop_table, KASUMI_FOP_HASH_BITS);
static DEFINE_HASHTABLE(kasumi_fop_templates, KASUMI_FOP_TEMPLATE_HASH_BITS);
static DEFINE_SPINLOCK(kasumi_fop_lock);
static LIST_HEAD(kasumi_fop_retiring);
static void kasumi_fop_reap(struct work_struct *work);
static DECLARE_WORK(kasumi_fop_reap_work, kasumi_fop_reap);
DEFINE_STATIC_SRCU(kasumi_fop_client_srcu);
static atomic_t kasumi_fop_live = ATOMIC_INIT(0);
static atomic_t kasumi_fop_retired = ATOMIC_INIT(0);
static bool kasumi_fop_override_ready;
static bool kasumi_fop_override_stopped;

KASUMI_NOCFI static int kasumi_shadow_iterate_shared(struct file *file,
						     struct dir_context *ctx);

static struct kasumi_fop_meta *kasumi_fop_lookup_rcu(struct inode *inode)
{
	struct kasumi_fop_meta *m;

	hash_for_each_possible_rcu(kasumi_fop_table, m, node,
				   (unsigned long)inode)
	{
		if (m->inode == inode)
			return m;
	}
	return NULL;
}

static void kasumi_fop_put_orig(const struct file_operations *fop)
{
	if (fop && fop->owner && fop->owner != THIS_MODULE)
		module_put(fop->owner);
}

static void kasumi_fop_meta_free(struct kasumi_fop_meta *m)
{
	if (!m)
		return;
	if (m->inode) {
		iput(m->inode);
		atomic_dec(&kasumi_fop_live);
	}
	if (m->retired)
		atomic_dec(&kasumi_fop_retired);
	if (m->sb)
		deactivate_super(m->sb);
	kfree(m);
}

static void kasumi_fop_template_free(struct kasumi_fop_template *template)
{
	if (!template)
		return;
	kasumi_fop_put_orig(template->orig_fop);
	kfree(template);
}

static struct kasumi_fop_template *
kasumi_fop_template_lookup_locked(const struct file_operations *orig)
{
	struct kasumi_fop_template *template;

	hash_for_each_possible(kasumi_fop_templates, template, node,
			       (unsigned long)orig)
	{
		if (template->orig_fop == orig)
			return template;
	}
	return NULL;
}

static struct kasumi_fop_template *
kasumi_fop_template_alloc(const struct file_operations *orig)
{
	struct kasumi_fop_template *template;

	template = kzalloc(sizeof(*template), GFP_KERNEL);
	if (!template)
		return ERR_PTR(-ENOMEM);
	if (orig->owner && orig->owner != THIS_MODULE &&
	    !try_module_get(orig->owner)) {
		kfree(template);
		return ERR_PTR(-ENODEV);
	}
	template->orig_fop = orig;
	template->shadow_fop = *orig;
	template->shadow_fop.owner = THIS_MODULE;
	template->shadow_fop.iterate_shared = kasumi_shadow_iterate_shared;
	template->ingress_fop = *orig;
	INIT_LIST_HEAD(&template->retire_node);
	return template;
}

static const struct file_operations *
kasumi_fop_installed_table(const struct kasumi_fop_meta *m)
{
	const struct kasumi_fop_template *template = m->template;

	return template->bridge.registered ? &template->ingress_fop
					   : &template->shadow_fop;
}

KASUMI_NOCFI static int kasumi_shadow_iterate_shared(struct file *file,
						     struct dir_context *ctx)
{
	struct kasumi_filldir_wrapper *wrapper;
	struct kasumi_fop_template *template;
	struct kasumi_fop_meta *m;
	const struct file_operations *fop;
	const struct file_operations *orig;
	kasumi_fop_iterate_client_fn client = NULL;
	void *client_data = NULL;
	int srcu_idx;
	int ret;

	if (!file)
		return -EINVAL;
	fop = READ_ONCE(file->f_op);
	if (!fop || fop->iterate_shared != kasumi_shadow_iterate_shared)
		return -ENOTDIR;
	template = container_of(fop, struct kasumi_fop_template, shadow_fop);
	orig = template->orig_fop;

	if (!orig || !orig->iterate_shared)
		return -ENOTDIR;

	atomic64_inc(&kasumi_hook_stats.iterate_fop_entries);
	srcu_idx = srcu_read_lock(&kasumi_fop_client_srcu);
	rcu_read_lock();
	m = kasumi_fop_lookup_rcu(file_inode(file));
	if (m && m->template == template) {
		/* Pairs with bind's release-store of the client pointer. */
		client = smp_load_acquire(&m->iterate_client);
		client_data = READ_ONCE(m->iterate_client_data);
	}
	rcu_read_unlock();
	if (client && client_data) {
		ret = client(file, ctx, orig, client_data);
		goto out;
	}

	wrapper = kasumi_iterate_prepare_wrapper(file, ctx);
	if (!wrapper) {
		ret = orig->iterate_shared(file, ctx);
		goto out;
	}

	atomic64_inc(&kasumi_hook_stats.iterate_fop_wrapped);
	ret = orig->iterate_shared(file, &wrapper->wrap_ctx);
	kasumi_iterate_finish_wrapper(wrapper);
out:
	srcu_read_unlock(&kasumi_fop_client_srcu, srcu_idx);
	return ret;
}

static void kasumi_fop_add_owner(struct kasumi_fop_meta *m, bool inject)
{
	if (inject) {
		m->inject_owners++;
		set_bit(AS_FLAGS_KASUMI_DIR_HAS_INJECT,
			&m->inode->i_mapping->flags);
	} else {
		m->iterate_owners++;
	}
}

KASUMI_NOCFI int kasumi_fop_get(struct inode *inode, bool inject)
{
	struct kasumi_fop_template *template, *spare;
	struct kasumi_fop_meta *m, *existing;
	const struct file_operations *orig;
	bool installed = false;
	int ret = 0;

	if (!READ_ONCE(kasumi_fop_override_ready))
		return -EOPNOTSUPP;
	if (!inode || !inode->i_mapping || !S_ISDIR(inode->i_mode))
		return -EINVAL;
	if (!inode->i_sb)
		return -EINVAL;
	spin_lock(&kasumi_fop_lock);
	existing = kasumi_fop_lookup_rcu(inode);
	if (existing && READ_ONCE(kasumi_fop_override_ready)) {
		kasumi_fop_add_owner(existing, inject);
		spin_unlock(&kasumi_fop_lock);
		return 0;
	}

	orig = READ_ONCE(inode->i_fop);
	spin_unlock(&kasumi_fop_lock);
	if (!orig || !orig->iterate_shared)
		return -EOPNOTSUPP;
	spare = kasumi_fop_template_alloc(orig);
	if (IS_ERR(spare))
		return PTR_ERR(spare);

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		kasumi_fop_template_free(spare);
		return -ENOMEM;
	}

	if (!atomic_inc_not_zero(&inode->i_sb->s_active)) {
		kfree(m);
		kasumi_fop_template_free(spare);
		return -ESTALE;
	}
	m->sb = inode->i_sb;
	m->inode = inode;
	atomic_inc(&kasumi_fop_live);
	INIT_LIST_HEAD(&m->retire_node);
	ihold(inode);

	spin_lock(&kasumi_fop_lock);
	if (!READ_ONCE(kasumi_fop_override_ready)) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	existing = kasumi_fop_lookup_rcu(inode);
	if (existing) {
		kasumi_fop_add_owner(existing, inject);
		ret = 0;
		goto out_unlock;
	}
	if (READ_ONCE(inode->i_fop) != orig) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	template = kasumi_fop_template_lookup_locked(orig);
	if (!template) {
		template = spare;
		hash_add_rcu(kasumi_fop_templates, &template->node,
			     (unsigned long)orig);
		(void)kasumi_fop_bridge_register(
		    &template->bridge, &template->ingress_fop,
		    &template->shadow_fop, template->orig_fop);
		spare = NULL;
	}
	m->template = template;
	kasumi_fop_add_owner(m, inject);
	hash_add_rcu(kasumi_fop_table, &m->node, (unsigned long)inode);
	set_bit(AS_FLAGS_KASUMI_FOP_INSTALLED, &inode->i_mapping->flags);
	smp_wmb();
	WRITE_ONCE(inode->i_fop, kasumi_fop_installed_table(m));
	installed = true;
	m = NULL;
out_unlock:
	spin_unlock(&kasumi_fop_lock);
	kasumi_fop_meta_free(m);
	kasumi_fop_template_free(spare);

	if (installed)
		kasumi_log("fop_override: installed on inode %p (orig=%p)\n",
			   inode, orig);
	return ret;
}

int kasumi_fop_bind_iterate_client(struct inode *inode,
				   kasumi_fop_iterate_client_fn client,
				   void *data)
{
	struct kasumi_fop_meta *m;
	int ret = 0;

	if (!inode || !client || !data)
		return -EINVAL;

	spin_lock(&kasumi_fop_lock);
	if (!READ_ONCE(kasumi_fop_override_ready)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	m = kasumi_fop_lookup_rcu(inode);
	if (!m) {
		ret = -ENOENT;
		goto out;
	}
	if (READ_ONCE(m->iterate_client_data) &&
	    (READ_ONCE(m->iterate_client_data) != data ||
	     (READ_ONCE(m->iterate_client) &&
	      READ_ONCE(m->iterate_client) != client))) {
		ret = -EBUSY;
		goto out;
	}

	/* Publish context before the callback.  Readers acquire-load the
	 * callback inside SRCU, so a non-NULL callback always observes
	 * initialized data. */
	WRITE_ONCE(m->iterate_client_data, data);
	smp_store_release(&m->iterate_client, client);
out:
	spin_unlock(&kasumi_fop_lock);
	return ret;
}

void kasumi_fop_unbind_iterate_client(struct inode *inode, void *data)
{
	struct kasumi_fop_meta *m;

	if (!inode)
		return;
	spin_lock(&kasumi_fop_lock);
	m = kasumi_fop_lookup_rcu(inode);
	if (m && (!data || READ_ONCE(m->iterate_client_data) == data))
		smp_store_release(&m->iterate_client, NULL);
	spin_unlock(&kasumi_fop_lock);
}

void kasumi_fop_synchronize_iterate_clients(void)
{
	synchronize_srcu(&kasumi_fop_client_srcu);
}

static struct kasumi_fop_meta *kasumi_fop_uninstall_locked(struct inode *inode)
{
	struct kasumi_fop_meta *m;

	m = kasumi_fop_lookup_rcu(inode);
	if (!m)
		return NULL;

	/* No new callback may acquire client data after this store.  A caller
	 * that frees the data waits for kasumi_fop_client_srcu below. */
	smp_store_release(&m->iterate_client, NULL);
	if (READ_ONCE(inode->i_fop) == kasumi_fop_installed_table(m)) {
		/* Publish the original table before the active meta is
		 * reclaimed. */
		smp_store_release(&inode->i_fop, m->template->orig_fop);
	}

	hash_del_rcu(&m->node);
	m->retired = true;
	atomic_inc(&kasumi_fop_retired);
	if (inode->i_mapping)
		clear_bit(AS_FLAGS_KASUMI_FOP_INSTALLED,
			  &inode->i_mapping->flags);
	if (inode->i_mapping)
		clear_bit(AS_FLAGS_KASUMI_DIR_HAS_INJECT,
			  &inode->i_mapping->flags);
	return m;
}

static void kasumi_fop_retire_list(struct list_head *retiring)
{
	struct kasumi_fop_meta *m, *next;

	if (list_empty(retiring))
		return;

	/* Dynamic fops templates are shared by original-fops identity and
	 * remain live until module exit.  A file opened across this restore can
	 * therefore finish fops_get()/the old-KMI bridge without retaining this
	 * per-inode client meta or creating one retired allocation per CLEAR
	 * generation.
	 */
	synchronize_rcu_tasks();
	/* Cover active-meta hash readers, then every callback which may have
	 * copied the client data before uninstall cleared it.
	 */
	synchronize_rcu();
	kasumi_fop_synchronize_iterate_clients();

	list_for_each_entry_safe (m, next, retiring, retire_node) {
		list_del_init(&m->retire_node);
		WRITE_ONCE(m->iterate_client_data, NULL);
		kasumi_fop_meta_free(m);
	}
}

static void kasumi_fop_reap(struct work_struct *work)
{
	LIST_HEAD(retiring);

	(void)work;
	for (;;) {
		spin_lock(&kasumi_fop_lock);
		list_splice_init(&kasumi_fop_retiring, &retiring);
		spin_unlock(&kasumi_fop_lock);
		if (list_empty(&retiring))
			return;
		kasumi_fop_retire_list(&retiring);
	}
}

void kasumi_fop_put(struct inode *inode, bool inject)
{
	struct kasumi_fop_meta *m;
	unsigned int *owners;

	if (!inode)
		return;
	spin_lock(&kasumi_fop_lock);
	m = kasumi_fop_lookup_rcu(inode);
	if (!m)
		goto out;
	owners = inject ? &m->inject_owners : &m->iterate_owners;
	if (WARN_ON_ONCE(!*owners))
		goto out;
	--*owners;
	if (!inject && !m->iterate_owners)
		WRITE_ONCE(m->iterate_client_data, NULL);
	if (inject && !m->inject_owners)
		clear_bit(AS_FLAGS_KASUMI_DIR_HAS_INJECT,
			  &m->inode->i_mapping->flags);
	if (!m->inject_owners && !m->iterate_owners) {
		m = kasumi_fop_uninstall_locked(inode);
		list_add_tail(&m->retire_node, &kasumi_fop_retiring);
		/* Materialization can release an owner inside iterate_shared.
		 */
		schedule_work(&kasumi_fop_reap_work);
	}
out:
	spin_unlock(&kasumi_fop_lock);
}

int kasumi_fop_live_count(void)
{
	return atomic_read(&kasumi_fop_live);
}

int kasumi_fop_retiring_count(void)
{
	return atomic_read(&kasumi_fop_retired);
}

int kasumi_fop_override_init(void)
{
	hash_init(kasumi_fop_table);
	hash_init(kasumi_fop_templates);
	WRITE_ONCE(kasumi_fop_override_stopped, false);
	WRITE_ONCE(kasumi_fop_override_ready, true);
	pr_info("kasumi: fop_override initialized\n");
	return 0;
}

static void kasumi_fop_override_retire_all(bool stop_new)
{
	LIST_HEAD(retiring);
	struct kasumi_fop_meta *m;
	struct hlist_node *tmp;
	bool reopen;
	int bkt;

	spin_lock(&kasumi_fop_lock);
	if (stop_new)
		WRITE_ONCE(kasumi_fop_override_stopped, true);
	reopen = READ_ONCE(kasumi_fop_override_ready) &&
		 !READ_ONCE(kasumi_fop_override_stopped);
	WRITE_ONCE(kasumi_fop_override_ready, false);
	hash_for_each_safe(kasumi_fop_table, bkt, tmp, m, node)
	{
		if (!stop_new && READ_ONCE(m->iterate_client))
			continue;
		m = kasumi_fop_uninstall_locked(m->inode);
		if (m)
			list_add_tail(&m->retire_node, &retiring);
	}
	spin_unlock(&kasumi_fop_lock);

	kasumi_fop_retire_list(&retiring);
	flush_work(&kasumi_fop_reap_work);

	/* Runtime CLEAR is reusable.  A concurrent/permanent stop wins and
	 * keeps the install gate closed. */
	if (reopen) {
		spin_lock(&kasumi_fop_lock);
		if (!READ_ONCE(kasumi_fop_override_stopped))
			WRITE_ONCE(kasumi_fop_override_ready, true);
		spin_unlock(&kasumi_fop_lock);
	}
}

void kasumi_fop_override_clear(void)
{
	kasumi_fop_override_retire_all(false);
}

void kasumi_fop_override_stop_new(void)
{
	kasumi_fop_override_retire_all(true);
}

void kasumi_fop_override_exit(void)
{
	LIST_HEAD(retired_templates);
	struct kasumi_fop_template *template, *next;
	struct hlist_node *tmp;
	int bkt;

	kasumi_fop_override_stop_new();

	spin_lock(&kasumi_fop_lock);
	WARN_ON_ONCE(!hash_empty(kasumi_fop_table));
	hash_for_each_safe(kasumi_fop_templates, bkt, tmp, template, node)
	{
		hash_del_rcu(&template->node);
		kasumi_fop_bridge_unregister(&template->bridge);
		list_add_tail(&template->retire_node, &retired_templates);
	}
	spin_unlock(&kasumi_fop_lock);

	/* PREPARE/module ownership guarantees no open shadow file can remain
	 * when module_exit runs. Finish bridge lookups before freeing the
	 * shared tables and releasing their one-per-original-fops owner
	 * references.
	 */
	synchronize_rcu();
	kasumi_fop_synchronize_iterate_clients();
	list_for_each_entry_safe (template, next, &retired_templates,
				  retire_node) {
		list_del_init(&template->retire_node);
		kasumi_fop_template_free(template);
	}
	pr_info("kasumi: fop_override exited\n");
}
