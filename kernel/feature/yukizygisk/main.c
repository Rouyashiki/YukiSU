#include <linux/compiler.h>
#include <linux/mutex.h>

#include "internal.h"
#include "klog.h" // IWYU pragma: keep

static DEFINE_MUTEX(yz_feature_lock);

int yz_feature_enable_early(void)
{
	int ret = 0;

	mutex_lock(&yz_feature_lock);
	if (!READ_ONCE(yukizygisk_enabled) && yz_early_native_active()) {
		ret = yz_process_exit_enable();
		if (!ret) {
			yz_load_policy_enable();
			ret = yz_exec_enable();
			if (ret) {
				yz_load_policy_disable();
				yz_process_exit_disable();
			}
		}
	}
	mutex_unlock(&yz_feature_lock);
	return ret;
}

int yz_feature_set_enabled(bool enabled)
{
	int ret = 0;

	mutex_lock(&yz_feature_lock);
	if (enabled && READ_ONCE(yukizygisk_enabled))
		goto out;

	if (enabled) {
		ret = yz_process_exit_enable();
		if (ret)
			goto out;
		yz_load_policy_enable();
		ret = yz_exec_enable();
		if (ret)
			goto undo_exit;

		ret = yz_lifecycle_enable();
		if (ret) {
			if (!yz_early_native_active())
				yz_exec_disable();
			goto undo_exit;
		}
		WRITE_ONCE(yukizygisk_enabled, true);
	} else {
		WRITE_ONCE(yukizygisk_enabled, false);
		yz_early_native_disable();
		yz_load_policy_disable();
		yz_exec_disable();
		yz_lifecycle_disable();
		yz_process_exit_disable();
	}

	pr_info("yukizygisk: enabled=%d\n", enabled);
	goto out;
undo_exit:
	if (!yz_early_native_active()) {
		yz_load_policy_disable();
		yz_process_exit_disable();
	}
out:
	mutex_unlock(&yz_feature_lock);
	return ret;
}

void ksu_yukizygisk_init(void)
{
	yz_exit_history_init();
	yz_events_init();
	yz_fd_handoff_init();
	yz_exec_init();
}

void ksu_yukizygisk_exit(void)
{
	yz_feature_set_enabled(false);
	yz_exec_exit();
	yz_load_policy_exit();
	yz_exit_history_exit();
	yz_events_exit();
	yz_fd_handoff_exit();
}
