#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;

#define PF_EXITING 1
#define PF_KTHREAD 2
#define FS_CREATE 1
#define FS_MOVED_TO 2
#define FS_ISDIR 4
#define GFP_KERNEL 0
#define TWA_RESUME 0
#define THIS_MODULE NULL
#define pr_info(...) ((void)0)
#define pr_warn(...) ((void)0)

struct callback_head {
	void (*func)(struct callback_head *cb);
};

struct fsnotify_mark;
struct inode;
struct qstr {
	u32 len;
	const char *name;
};

static struct {
	unsigned int flags;
} task;
#define current (&task)

static struct callback_head *pending_work;
static unsigned int allocations;
static unsigned int module_refs;
static unsigned int tracker_calls;
static bool fail_allocation;
static bool fail_queue;
static bool module_available = true;
static bool last_prune_only;
static bool last_force_scan;
static int throne_tracker_mutex;

static void *kzalloc(size_t size, int flags)
{
	void *ptr;

	(void)flags;
	if (fail_allocation)
		return NULL;
	ptr = calloc(1, size);
	assert(ptr);
	allocations++;
	return ptr;
}

static void kfree(void *ptr)
{
	assert(ptr && allocations);
	allocations--;
	free(ptr);
}

static bool try_module_get(void *module)
{
	(void)module;
	if (!module_available)
		return false;
	module_refs++;
	return true;
}

static void module_put(void *module)
{
	(void)module;
	assert(module_refs);
	module_refs--;
}

static int task_work_add(void *owner, struct callback_head *cb, int mode)
{
	assert(owner == current && mode == TWA_RESUME);
	if (fail_queue)
		return -1;
	assert(!pending_work);
	pending_work = cb;
	return 0;
}

static void mutex_lock(int *lock)
{
	assert(*lock == 0);
	*lock = 1;
}

static void mutex_unlock(int *lock)
{
	assert(*lock == 1);
	*lock = 0;
}

static void track_throne_locked(bool prune_only, bool force_scan)
{
	assert(throne_tracker_mutex == 1);
	tracker_calls++;
	last_prune_only = prune_only;
	last_force_scan = force_scan;
}

#include "pkg_observer_impl.c"

static void reset_fixture(void)
{
	assert(!pending_work && !allocations && !module_refs);
	assert(throne_tracker_mutex == 0);
	task.flags = 0;
	tracker_calls = 0;
	fail_allocation = false;
	fail_queue = false;
	module_available = true;
	last_prune_only = false;
	last_force_scan = false;
}

static void package_event(u32 mask)
{
	const struct qstr name = {13, "packages.list"};

	assert(ksu_handle_inode_event(NULL, mask, NULL, NULL, &name, 0) == 0);
}

static void run_task_work(void)
{
	struct callback_head *cb = pending_work;

	assert(cb);
	pending_work = NULL;
	cb->func(cb);
}

static void assert_full_scan(void)
{
	assert(tracker_calls == 1);
	assert(!last_prune_only && last_force_scan);
	assert(!pending_work && !allocations && !module_refs);
	assert(throne_tracker_mutex == 0);
}

int main(void)
{
	const struct qstr unrelated = {12, "packages.xml"};
	int failure;

	reset_fixture();
	package_event(FS_MOVED_TO);
	assert(!tracker_calls && pending_work && module_refs == 1);
	run_task_work();
	assert_full_scan();

	for (failure = 0; failure < 3; failure++) {
		reset_fixture();
		fail_allocation = failure == 0;
		fail_queue = failure == 1;
		task.flags = failure == 2 ? PF_KTHREAD : 0;
		package_event(FS_CREATE);
		assert_full_scan();
	}

	reset_fixture();
	package_event(FS_MOVED_TO);
	task.flags |= PF_EXITING;
	run_task_work();
	assert(!tracker_calls && !allocations && !module_refs);

	reset_fixture();
	module_available = false;
	package_event(FS_CREATE);
	assert(!tracker_calls && !pending_work && !allocations && !module_refs);

	reset_fixture();
	package_event(FS_CREATE | FS_ISDIR);
	assert(ksu_handle_inode_event(NULL, FS_CREATE, NULL, NULL, &unrelated,
				      0) == 0);
	assert(ksu_handle_inode_event(NULL, FS_CREATE, NULL, NULL, NULL, 0) ==
	       0);
	assert(!tracker_calls && !pending_work);

	track_throne(true);
	assert(tracker_calls == 1 && last_prune_only && !last_force_scan);
	reset_fixture();
	puts("pkg_observer_test: all checks passed");
	return 0;
}
