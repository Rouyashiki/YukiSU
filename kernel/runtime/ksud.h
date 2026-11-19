#ifndef __KSU_H_KSUD
#define __KSU_H_KSUD

#include <linux/types.h>

struct pt_regs;

#define KSUD_PATH "/data/adb/ksud"

void ksu_ksud_init(void);
void ksu_ksud_exit(void);

bool ksu_is_safe_mode(void);
bool ksu_is_safe_mode_requested(void);
void ksu_stop_input_hook_runtime(void);

extern u32 ksu_file_sid;

#define MAX_ARG_STRINGS 0x7FFFFFFF
struct user_arg_ptr {
	const char __user *const __user *native;
};

void ksu_handle_execveat_ksud(const char *filename, struct user_arg_ptr *argv,
			      struct user_arg_ptr *envp, int *flags);
void ksu_execve_hook_ksud(const struct pt_regs *regs);
void ksu_execveat_hook_ksud(const struct pt_regs *regs);

#endif // #ifndef __KSU_H_KSUD
