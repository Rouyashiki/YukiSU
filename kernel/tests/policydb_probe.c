#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "ss/services.h"
#include "ss/avtab.h"
#include "ss/ebitmap.h"
#include "ss/hashtab.h"
#include "policydb_cleanup.h"
#include "policydb_alloc.h"
#include "fixture.h"

#define MAX_ALLOCATIONS 131072
struct allocation {
	const void *ptr;
	struct kmem_cache *cache;
	unsigned int owner;
	bool virtual;
};
static struct allocation ledger[MAX_ALLOCATIONS];
static unsigned int used, live, owner, attempts, fail_at;
static unsigned int original_live, early_errors, clean_errors;
static size_t fail_size;
static bool hit_failure, baseline;
static const char *failed_site;
static unsigned int snapshot_writes, snapshot_reads, fail_snapshot_read;

static void check(bool condition, const char *message)
{
	if (!condition)
		panic("POLICYDB_TEST_FAIL: %s owner=%u fail_at=%u site=%s "
		      "live=%u\n",
		      message, owner, fail_at, failed_site ?: "none", live);
}

void *test_alloc(size_t size, gfp_t flags, struct kmem_cache *cache,
		 bool virtual, const char *site)
{
	void *ptr;

	if (!size && !cache)
		return ZERO_SIZE_PTR;
	attempts++;
	if ((fail_at && attempts == fail_at) ||
	    (fail_size && size >= fail_size && !hit_failure)) {
		hit_failure = true;
		failed_site = site;
		return NULL;
	}
	ptr = cache	? kmem_cache_alloc(cache, flags)
	      : virtual ? kvmalloc(size, flags)
			: kmalloc(size, flags);
	if (!ZERO_OR_NULL_PTR(ptr)) {
		check(used < MAX_ALLOCATIONS, "allocation ledger overflow");
		ledger[used++] =
		    (struct allocation){ptr, cache, owner, virtual};
		live++;
	}
	return ptr;
}

void test_free(const void *ptr, struct kmem_cache *cache, bool virtual)
{
	unsigned int i;

	if (ZERO_OR_NULL_PTR(ptr))
		return;
	for (i = used; i > 0; i--) {
		struct allocation *a = &ledger[i - 1];
		if (a->ptr != ptr)
			continue;
		check(a->owner == owner, "freed another policy's allocation");
		check(a->cache == cache, "wrong slab cache");
		check(virtual || !a->virtual,
		      "vmalloc allocation passed to kfree");
		a->ptr = NULL;
		live--;
		if (cache)
			kmem_cache_free(cache, (void *)ptr);
		else if (virtual)
			kvfree(ptr);
		else
			kfree(ptr);
		return;
	}
	check(false, "double free or untracked allocation");
}

void *test_copy(const void *ptr, size_t size, gfp_t flags, const char *site)
{
	void *copy = test_alloc(size, flags, NULL, false, site);

	if (copy)
		memcpy(copy, ptr, size);
	return copy;
}

static int test_policydb_write(struct policydb *db, struct policy_file *fp)
{
	snapshot_writes++;
	return policydb_write(db, fp);
}

static int test_policydb_read(struct policydb *db, struct policy_file *fp)
{
	int ret;

	if (++snapshot_reads != fail_snapshot_read)
		return policydb_read(db, fp);

	fail_at = attempts + 1;
	ret = policydb_read(db, fp);
	fail_at = 0;
	check(hit_failure && ret == -ENOMEM,
	      "selected read did not fail with ENOMEM");
	return ret;
}

#include "clone_impl.c"

static void reset_attempt(unsigned int nth)
{
	used = original_live;
	attempts = 0;
	fail_at = nth;
	failed_site = NULL;
	hit_failure = false;
	owner++;
}

#ifdef TEST_INITIAL_SNAPSHOT
#include "initial_sid_to_string.h"

const char *security_get_initial_sid_context(u32 sid)
{
	if (sid >= ARRAY_SIZE(initial_sid_to_string))
		return NULL;
	return initial_sid_to_string[sid];
}

static struct selinux_state test_state;
static DEFINE_MUTEX(ksu_backup_mutex);
static struct selinux_policy *backup_sepolicy;

#define selinux_state test_state
#define ksu_dup_sepolicy test_dup
#define ksu_policy_decision(...) ((void)0)
#include "initial_snapshot_impl.c"
#undef ksu_policy_decision
#undef ksu_dup_sepolicy
#undef selinux_state

static void exercise_initial_snapshot(struct selinux_policy *old)
{
	struct selinux_policy *copy, *backup;
	unsigned int failed_read;

	mutex_init(&test_state.policy_mutex);
	for (failed_read = 0; failed_read <= 2; failed_read++) {
		reset_attempt(0);
		snapshot_writes = snapshot_reads = 0;
		fail_snapshot_read = failed_read;
		mutex_lock(&test_state.policy_mutex);
		mutex_lock(&ksu_backup_mutex);
		copy = dup_policy_for_kernel_rules(old);
		mutex_unlock(&ksu_backup_mutex);
		mutex_unlock(&test_state.policy_mutex);

		check(snapshot_writes == 1 && snapshot_reads == 2,
		      "initial load must write once and read twice");
		backup = ksu_detach_backup_sepolicy();
		if (failed_read == 1) {
			check(!backup, "failed backup was published");
		} else {
			check(!IS_ERR_OR_NULL(backup),
			      "initial backup missing");
			check(backup->sidtab && backup->sidtab != old->sidtab,
			      "initial backup lacks a private SID table");
			check(sidtab_search(backup->sidtab, SECINITSID_KERNEL),
			      "initial backup lost the kernel SID");
			check(
			    symtab_search(&backup->policydb.p_types, "target"),
			    "initial backup type lookup failed");
			sidtab_destroy(backup->sidtab);
			test_kfree(backup->sidtab);
			ksu_destroy_sepolicy(backup);
		}
		if (failed_read == 2) {
			check(PTR_ERR(copy) == -ENOMEM,
			      "writable read lost its error");
		} else {
			check(!IS_ERR_OR_NULL(copy),
			      "initial writable copy missing");
			check(copy->sidtab == old->sidtab,
			      "initial writable copy lost the borrowed SID "
			      "table");
			check(symtab_search(&copy->policydb.p_types, "target"),
			      "initial writable type lookup failed");
			ksu_destroy_sepolicy(copy);
		}
		check(live == original_live,
		      "initial load retained allocations");
	}
	fail_snapshot_read = 0;
	pr_info("POLICYDB_INITIAL_SNAPSHOT_PASS\n");
}
#endif

static void parse_case(const unsigned char *data, size_t len, unsigned int nth,
		       bool legacy)
{
	struct policydb *p = kzalloc(sizeof(*p), GFP_KERNEL);
	struct policy_file fp = {.data = (void *)data, .len = len};
	int ret;

	check(p != NULL, "test policy allocation");
	reset_attempt(nth);
	ret = policydb_read(p, &fp);
	fail_at = 0;
	if (hit_failure)
		check(ret != 0, "allocation failure reported as success");
	if (!ret) {
		policydb_destroy(p);
	} else if (policydb_read_needs_destroy(p)) {
		early_errors++;
		if (legacy)
			check(live > original_live,
			      "baseline failed to reproduce leak");
		policydb_destroy(p);
	} else {
		clean_errors++;
	}
	check(live == original_live, "parser retained allocations");
	kfree(p);
}

static void exercise_policy(const unsigned char *data, size_t len)
{
	struct selinux_policy old = {};
	struct selinux_policy *copy;
	struct policy_file fp = {.data = (void *)data, .len = len};
	unsigned int total, nth;
	int ret;

	owner = used = live = original_live = fail_at = attempts = 0;
	ret = policydb_read(&old.policydb, &fp);
	check(ret == 0, "fixture is not a valid kernel policy");
	total = attempts;
	old.policydb.len = len;
	old.sidtab = (void *)0x12340000UL;
	original_live = live;
	for (nth = 0, total = 0; nth < used; nth++) {
		if (ledger[nth].ptr)
			ledger[total++] = ledger[nth];
	}
	total = attempts;
	for (nth = 1; nth <= total; nth++)
		parse_case(data, len, nth, baseline);
	parse_case(data, len, 0, false);
	for (nth = 0; nth < len; nth++)
		parse_case(data, nth, 0, false);

	reset_attempt(0);
	copy = test_dup(&old);
	check(!IS_ERR(copy), "production clone success");
	total = attempts;
	check(copy->sidtab == old.sidtab, "borrowed SID table changed");
	policydb_destroy(&copy->policydb);
	test_free(copy, NULL, false);
	check(live == original_live, "successful clone leaked");
	for (nth = 1; nth <= total; nth++) {
		reset_attempt(nth);
		copy = test_dup(&old);
		fail_at = 0;
		check(hit_failure && IS_ERR(copy),
		      "clone failed allocation ignored");
		check(live == original_live, "failed clone leaked");
	}
#ifdef TEST_INITIAL_SNAPSHOT
	old.latest_granting = 1;
	exercise_initial_snapshot(&old);
#endif
	owner = 0;
	policydb_destroy(&old.policydb);
	check(live == 0, "original policy leaked");
	pr_info("POLICYDB_FIXTURE_PASS bytes=%zu clone_allocations=%u\n", len,
		total);
}

static int __init policydb_probe_init(void)
{
	ebitmap_cache_init();
	hashtab_cache_init();
	avtab_cache_init();
	baseline = true;
	exercise_policy(fixture, sizeof(fixture));
#ifdef TEST_FIXED_PARSER
	check(early_errors == 0 && clean_errors > 0,
	      "fixed parser cleanup contract");
#else
	check(early_errors > 0 && clean_errors > 0, "missing cleanup branches");
#endif
	original_live = used = live = 0;
	fail_size = 65536;
	parse_case(large_fixture, sizeof(large_fixture), 0, true);
	check(hit_failure && !strcmp(failed_site, "probe_hashtab_init"),
	      "64 KiB symbol table allocation was not exercised");
	fail_size = 0;
	pr_info("POLICYDB_TEST_PASS early=%u already_clean=%u\n", early_errors,
		clean_errors);
	return 0;
}

static void __exit policydb_probe_exit(void)
{
}
module_init(policydb_probe_init);
module_exit(policydb_probe_exit);
MODULE_DESCRIPTION("SELinux policy database allocation and snapshot tests");
MODULE_LICENSE("GPL");
