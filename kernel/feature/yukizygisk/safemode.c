#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "api.h"
#include "internal.h"
#include "klog.h" // IWYU pragma: keep
#include "uapi/yukizygisk.h"

struct yz_zygote_guard {
	char name[YZ_ZYGOTE_NAME_MAX];
	pid_t last_pid;
	u32 zygote_crashes;
};

#define YZ_ZYGOTE_GUARD_MAX 4
static DEFINE_SPINLOCK(yz_safemode_lock);
static struct yz_zygote_guard yz_zygote_guards[YZ_ZYGOTE_GUARD_MAX];
static bool yz_safemode_active;
static u32 yz_safemode_zygote_crashes;
static char yz_safemode_zygote[YZ_ZYGOTE_NAME_MAX];

bool yz_safemode_is_active(void)
{
	return READ_ONCE(yz_safemode_active);
}

static int yz_zygote_guard_slot_locked(const char *name, int *free_slot)
{
	int i;

	if (free_slot)
		*free_slot = -1;

	for (i = 0; i < YZ_ZYGOTE_GUARD_MAX; i++) {
		if (yz_zygote_guards[i].name[0] == '\0') {
			if (free_slot && *free_slot < 0)
				*free_slot = i;
			continue;
		}
		if (!strcmp(yz_zygote_guards[i].name, name))
			return i;
	}
	return -1;
}

bool yz_zygote_safemode_should_skip(const char *name)
{
	unsigned long flags;
	char zygote[YZ_ZYGOTE_NAME_MAX];
	pid_t pid = current->tgid;
	u32 crashes = 0;
	bool activate = false;
	bool skip = false;
	int free_slot;
	int slot;

	yz_copy_name(zygote, sizeof(zygote),
		     (name && name[0]) ? name : "zygote");

	spin_lock_irqsave(&yz_safemode_lock, flags);
	if (yz_safemode_active) {
		skip = true;
		crashes = yz_safemode_zygote_crashes;
	} else {
		slot = yz_zygote_guard_slot_locked(zygote, &free_slot);
		if (slot < 0 && free_slot >= 0) {
			slot = free_slot;
			yz_copy_name(yz_zygote_guards[slot].name,
				     sizeof(yz_zygote_guards[slot].name),
				     zygote);
		}
		if (slot >= 0) {
			struct yz_zygote_guard *guard = &yz_zygote_guards[slot];

			if (guard->last_pid == 0) {
				guard->last_pid = pid;
			} else if (guard->last_pid != pid) {
				guard->last_pid = pid;
				guard->zygote_crashes++;
				crashes = guard->zygote_crashes;
				if (crashes >= YZ_ZYGOTE_CRASH_THRESHOLD) {
					WRITE_ONCE(yz_safemode_active, true);
					yz_safemode_zygote_crashes = crashes;
					yz_copy_name(yz_safemode_zygote,
						     sizeof(yz_safemode_zygote),
						     zygote);
					activate = true;
					skip = true;
				}
			}
		}
	}
	spin_unlock_irqrestore(&yz_safemode_lock, flags);

	if (activate) {
		pr_warn("yukizygisk: safe mode enabled zygote=%s restarts=%u "
			"pid=%d\n",
			zygote, crashes, pid);
		yz_emit_safemode((u32)pid, crashes);
	} else if (skip) {
		pr_info("yukizygisk: safe mode skipped zygote pid=%d name=%s "
			"restarts=%u\n",
			pid, zygote, crashes);
	}
	return skip;
}

int ksu_yukizygisk_get_safemode(struct yz_safemode_status_cmd *cmd)
{
	unsigned long flags;

	if (!cmd)
		return -EINVAL;

	memset(cmd, 0, sizeof(*cmd));
	spin_lock_irqsave(&yz_safemode_lock, flags);
	cmd->active = yz_safemode_active ? 1 : 0;
	cmd->zygote_crashes = yz_safemode_zygote_crashes;
	yz_copy_name(cmd->zygote, sizeof(cmd->zygote), yz_safemode_zygote);
	spin_unlock_irqrestore(&yz_safemode_lock, flags);
	return 0;
}

void yz_safemode_fill_runtime_query(struct yz_runtime_query_cmd *query)
{
	unsigned long flags;

	spin_lock_irqsave(&yz_safemode_lock, flags);
	query->safe_mode = yz_safemode_active ? 1 : 0;
	query->zygote_crashes = yz_safemode_zygote_crashes;
	query->capabilities = YZ_RUNTIME_CAP_MODULE_IMAGE_POLICY;
	yz_copy_name(query->safe_mode_zygote, sizeof(query->safe_mode_zygote),
		     yz_safemode_zygote);
	spin_unlock_irqrestore(&yz_safemode_lock, flags);
}
