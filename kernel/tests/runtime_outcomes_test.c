#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "uapi/yukizygisk.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
struct yz_runtime_slot {
	struct yz_runtime_record record;
	u64 start_boottime;
	int pid_node;
};
static struct yz_runtime_slot slots[16];
static u64 starts[4];
static unsigned int updates;
#define hash_for_each_possible(table, slot, node, key)                         \
	for (slot = slots; slot < slots + 16; slot++)
#define spin_lock_irqsave(lock, flags) ((void)(flags = 0))
#define spin_unlock_irqrestore(lock, flags) ((void)flags)
#define hash_del(node) ((void)0)
#define hash_add(table, node, key) ((void)0)

static int yz_runtime_get_task_start(u32 pid, u64 *start)
{
	if (pid >= 4 || !starts[pid])
		return -ESRCH;
	*start = starts[pid];
	return 0;
}
static bool yz_runtime_task_alive(u32 pid, u64 start)
{
	return pid < 4 && starts[pid] == start;
}
static void yz_runtime_advance_locked(void)
{
	updates++;
}
static void yz_runtime_refresh_exited_locked(void)
{
}
static void yz_runtime_refresh_pid_locked(u32 pid, bool *checked)
{
}
static struct yz_runtime_slot *
yz_runtime_alloc_slot_locked(struct yz_runtime_slot *base)
{
	for (unsigned int i = 0; i < 16; i++) {
		if (!slots[i].record.pid ||
		    slots[i].record.state == YZ_RUNTIME_STATE_EXITED)
			return &slots[i];
	}
	return NULL;
}
static void yz_copy_name(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src);
}

#include "report_impl.c"

static struct yz_runtime_report_cmd report(u32 pid, u32 generation, u8 state)
{
	struct yz_runtime_report_cmd r = {
	    .pid = pid,
	    .generation = generation,
	    .kind = YZ_RUNTIME_KIND_ZYGOTE,
	    .module_state = state,
	};
	strcpy(r.module_id, "zygisk_vector");
	return r;
}
static struct yz_runtime_slot *module(u32 pid, u32 generation)
{
	for (unsigned int i = 0; i < 16; i++) {
		if (slots[i].record.pid == pid &&
		    slots[i].record.generation == generation &&
		    !strcmp(slots[i].record.module_id, "zygisk_vector"))
			return &slots[i];
	}
	assert(false);
	return NULL;
}
static void base(unsigned int index, u32 pid, u32 generation, u8 abi)
{
	slots[index] = (struct yz_runtime_slot){
	    .record = {.pid = pid,
		       .generation = generation,
		       .abi = abi,
		       .kind = YZ_RUNTIME_KIND_ZYGOTE,
		       .state = YZ_RUNTIME_STATE_INJECTED},
	    .start_boottime = starts[pid],
	};
}
int main(void)
{
	for (unsigned int reverse = 0; reverse < 2; reverse++) {
		memset(slots, 0, sizeof(slots));
		starts[1] = 10;
		starts[2] = 20;
		base(0, 1, 1, YZ_RUNTIME_ABI_64);
		base(1, 2, 1, YZ_RUNTIME_ABI_32);
		struct yz_runtime_report_cmd ok =
		    report(1, 1, YZ_RUNTIME_STATE_INJECTED);
		struct yz_runtime_report_cmd bad =
		    report(1, 1, YZ_RUNTIME_STATE_FAILED);
		assert(test_report(reverse ? &bad : &ok) == 0);
		unsigned int before = updates;
		assert(test_report(reverse ? &bad : &ok) == 0);
		assert(updates == before);
		assert(test_report(reverse ? &ok : &bad) == 0);
		assert(updates == before + 1);
		assert(module(1, 1)->record.flags ==
		       YZ_RUNTIME_F_MODULE_OUTCOMES);
		assert(module(1, 1)->record.state == YZ_RUNTIME_STATE_FAILED);
		before = updates;
		assert(test_report(&ok) == 0 && test_report(&bad) == 0);
		assert(updates == before);

		struct yz_runtime_report_cmd other = report(2, 1, 0);
		assert(test_report(&other) == 0);
		assert(module(2, 1)->record.flags ==
		       YZ_RUNTIME_F_MODULE_SUCCESS);
		assert(module(2, 1)->record.abi == YZ_RUNTIME_ABI_32);
		other.generation = 99;
		assert(test_report(&other) == -ESRCH);
		starts[1] = 30;
		assert(test_report(&ok) == -ESRCH);
		base(0, 1, 2, YZ_RUNTIME_ABI_64);
		ok.generation = 2;
		assert(test_report(&ok) == 0);
		assert(module(1, 2)->record.flags ==
		       YZ_RUNTIME_F_MODULE_SUCCESS);
		assert(module(1, 1)->record.flags ==
		       YZ_RUNTIME_F_MODULE_OUTCOMES);
		slots[0].record.state = YZ_RUNTIME_STATE_EXITED;
		assert(test_report(&ok) == -ESRCH);
		base(0, 1, 3, YZ_RUNTIME_ABI_64);
		ok.generation = 3;
		ok.module_state = YZ_RUNTIME_STATE_SAFEMODE;
		assert(test_report(&ok) == 0);
		ok.module_state = YZ_RUNTIME_STATE_INJECTED;
		assert(test_report(&ok) == 0);
		assert(module(1, 3)->record.state == YZ_RUNTIME_STATE_SAFEMODE);
		assert(module(1, 3)->record.flags ==
		       YZ_RUNTIME_F_MODULE_SUCCESS);
	}
	puts("PASS: production runtime reports, ordering, generations, PID "
	     "reuse and ABI isolation");
}
