#ifndef __KSU_H_SUPERCALL_INTERNAL
#define __KSU_H_SUPERCALL_INTERNAL

#include <linux/types.h>
#include <linux/uaccess.h>

#include "supercall/supercall.h"

struct kasumi_transaction_state;

struct ksu_driver_context {
	unsigned long permissions;
	struct kasumi_transaction_state *transaction;
};

bool only_manager(void);
bool only_root(void);
bool require_load_policy_perm(void);
bool manager_or_root(void);
bool always_allow(void);
bool allowed_for_su(void);
bool injected_app(void);

long ksu_supercall_handle_ioctl(const struct file *filp, unsigned int cmd,
				void __user *argp);
void ksu_supercall_dump_commands(void);

typedef int (*ksu_ioctl_handler_t)(void __user *arg);
typedef bool (*ksu_perm_check_t)(void);

struct ksu_ioctl_cmd_map {
	unsigned int cmd;
	const char *name;
	ksu_ioctl_handler_t handler;
	ksu_perm_check_t perm_check;
	bool allow_su_session;
};

#endif // #ifndef __KSU_H_SUPERCALL_INTERNAL
