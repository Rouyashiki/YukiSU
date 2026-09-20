#ifndef __KSU_YUKIZYGISK_API_H
#define __KSU_YUKIZYGISK_API_H

#include <linux/types.h>

struct cred;
struct file;
struct pt_regs;
struct yz_native_targets_cmd;
struct yz_runtime_query_cmd;
struct yz_runtime_record;
struct yz_runtime_report_cmd;
struct yz_safemode_status_cmd;

void ksu_yukizygisk_init(void);
void ksu_yukizygisk_exit(void);
void ksu_yukizygisk_observe_execve(const struct pt_regs *regs);
void ksu_yukizygisk_observe_execveat(const struct pt_regs *regs);

int ksu_yukizygisk_handoff_module_fds(void __user *arg);
void ksu_yukizygisk_emit_reload(void);
void ksu_yukizygisk_on_setresuid(uid_t old_uid, uid_t new_uid);

void ksu_yukizygisk_set_linker_offsets(u64 dlopen_off, u64 dlsym_off);
void ksu_yukizygisk_set_compat_linker_offsets(u64 dlopen_off, u64 dlsym_off);
void ksu_yukizygisk_set_first_stage_loader(bool enabled);
int ksu_yukizygisk_set_native_targets(const struct yz_native_targets_cmd *cmd);
int ksu_yukizygisk_restore_native_load_policy(pid_t tgid);
bool ksu_yukizygisk_is_native_runtime(pid_t pid, u64 start_boottime);
int ksu_yukizygisk_allow_module_load_policy(pid_t tgid, struct file *dir,
					    const struct cred *cred);
int ksu_yukizygisk_get_safemode(struct yz_safemode_status_cmd *cmd);
int ksu_yukizygisk_get_runtime(struct yz_runtime_record *entries, u32 capacity,
			       struct yz_runtime_query_cmd *query);
int ksu_yukizygisk_report_runtime(const struct yz_runtime_report_cmd *report);

#endif
