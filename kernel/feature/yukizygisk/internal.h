#ifndef __KSU_YUKIZYGISK_INTERNAL_H
#define __KSU_YUKIZYGISK_INTERNAL_H

#include <linux/types.h>

#include "api.h"
#include "uapi/yukizygisk.h"

struct file;
struct ksu_feature_handler;
struct ksu_file_load_policy;
struct mm_struct;
struct task_struct;

#define YZ_LOADER64_NAME "libyukilinker64.so"
#define YZ_LOADER32_NAME "libyukilinker32.so"
#define YZ_CORE64_NAME "libzygisk64.so"
#define YZ_CORE32_NAME "libzygisk32.so"
#define YZ_NATIVE_CORE64_NAME "libyukizncore64.so"
#define YZ_NATIVE_CORE32_NAME "libyukizncore32.so"
#define YZ_RUNTIME_DIR "/data/adb/ksu/lib/yukizygisk/"
#define YZ_LOADER64_PATH YZ_RUNTIME_DIR YZ_LOADER64_NAME
#define YZ_LOADER32_PATH YZ_RUNTIME_DIR YZ_LOADER32_NAME
#define YZ_CORE64_PATH YZ_RUNTIME_DIR YZ_CORE64_NAME
#define YZ_CORE32_PATH YZ_RUNTIME_DIR YZ_CORE32_NAME
#define YZ_NATIVE_CORE64_PATH YZ_RUNTIME_DIR YZ_NATIVE_CORE64_NAME
#define YZ_NATIVE_CORE32_PATH YZ_RUNTIME_DIR YZ_NATIVE_CORE32_NAME
#define YZ_SYSTEM_LINKER64 "/system/bin/linker64"
#define YZ_SYSTEM_LINKER32 "/system/bin/linker"
#define YZ_EARLY_MANIFEST_WATCHDOG                                             \
	"/metadata/watchdog/ksu/yukizygisk/native_snapshot.bin"
#define YZ_EARLY_MANIFEST_DEFAULT "/metadata/ksu/yukizygisk/native_snapshot.bin"
#define YZ_EARLY_LOADER64_WATCHDOG                                             \
	"/metadata/watchdog/ksu/yukizygisk/libyukilinker64.so"
#define YZ_EARLY_LOADER64_DEFAULT "/metadata/ksu/yukizygisk/libyukilinker64.so"
#define YZ_EARLY_LOADER32_WATCHDOG                                             \
	"/metadata/watchdog/ksu/yukizygisk/libyukilinker32.so"
#define YZ_EARLY_LOADER32_DEFAULT "/metadata/ksu/yukizygisk/libyukilinker32.so"
#define YZ_EARLY_NATIVE_CORE64_WATCHDOG                                        \
	"/metadata/watchdog/ksu/yukizygisk/libyukizncore64.so"
#define YZ_EARLY_NATIVE_CORE64_DEFAULT                                         \
	"/metadata/ksu/yukizygisk/libyukizncore64.so"
#define YZ_EARLY_NATIVE_CORE32_WATCHDOG                                        \
	"/metadata/watchdog/ksu/yukizygisk/libyukizncore32.so"
#define YZ_EARLY_NATIVE_CORE32_DEFAULT                                         \
	"/metadata/ksu/yukizygisk/libyukizncore32.so"
#define YZ_VMA_NAME "memfd:"
#define YZ_VMA_NAME_LEN sizeof(YZ_VMA_NAME)
#define YZ_LOADER_MAX_SZ (8u << 20) /* sanity cap on a payload image */
#define YZ_DLEXT_USE_LIBRARY_FD 0x10 /* android_dlextinfo.flags bit */
#define YZ_DLEXT_FORCE_LOAD 0x40

extern bool yukizygisk_enabled;
extern bool yz_yukilinker_enabled;
extern u64 yz_dlopen_off;
extern u64 yz_dlsym_off;
extern u64 yz_dlopen32_off;
extern u64 yz_dlsym32_off;
extern const struct ksu_feature_handler yukizygisk_feature_handler;

int yz_feature_set_enabled(bool enabled);
int yz_feature_enable_early(void);
void yz_exec_init(void);
void yz_exec_exit(void);
int yz_exec_enable(void);
void yz_exec_disable(void);
void yz_events_init(void);
void yz_events_exit(void);
void yz_emit_specialize(u32 pid, u32 appid);
void yz_emit_safemode(u32 pid, u32 crashes);
void yz_emit_zygote_exit(const struct yz_zygote_exit_event *event);
void yz_emit_target_exit(const struct yz_target_exit_event *event);
int yz_process_exit_enable(void);
void yz_process_exit_disable(void);
void yz_process_exit_schedule(void);
bool yz_process_exit_active(void);
bool yz_runtime_on_exit(struct task_struct *task);
void yz_runtime_reconcile(void);
bool yz_runtime_take_exit(struct yz_target_exit_event *event);
void yz_lifecycle_on_exit(struct task_struct *task);
void yz_load_policy_reap(void);
bool yz_load_policy_has_owner(struct task_struct *task);
int yz_lifecycle_enable(void);
void yz_lifecycle_disable(void);
void yz_fd_handoff_init(void);
void yz_fd_handoff_exit(void);
void yz_fd_handoff_release(struct task_struct *task);
void yz_fd_handoff_cancel_all(void);

struct yz_early_packet_state {
	int packet_fd;
	int module_fds[YZ_NATIVE_TARGET_MAX];
	u32 module_fd_count;
};

const char *yz_basename(const char *path);
void yz_copy_name(char *dst, size_t dst_len, const char *src);
bool yz_match_live_native_target(const char *filename, char *label,
				 size_t label_len, u8 *target_type);

void yz_restore_native_policy_state(struct ksu_file_load_policy *state);
void yz_publish_native_policy_state(pid_t tgid,
				    struct ksu_file_load_policy *state);
void yz_cleanup_module_policies(void);

bool yz_safemode_is_active(void);
bool yz_zygote_safemode_should_skip(const char *name);
void yz_safemode_fill_runtime_query(struct yz_runtime_query_cmd *query);
void yz_runtime_read_process(struct mm_struct *mm, char *process,
			     size_t process_len);
u32 yz_runtime_begin(u8 kind, u8 abi, u8 target_type, u32 flags,
		     const char *process, const char *target,
		     u64 start_boottime);
void yz_runtime_set_state(u32 pid, u32 generation, u8 state);
bool yz_parse_zygote_args(struct mm_struct *mm, char *socket_name,
			  size_t socket_name_len);

bool yz_early_native_active(void);
void yz_early_native_disable(void);
bool yz_match_early_native_target(const char *filename, char *label,
				  size_t label_len, u8 *target_type);
const char *yz_early_loader_path(bool compat);
const char *yz_early_native_core_path(bool compat);
void yz_close_current_fd(int fd);
void yz_cache_name(char *buf, size_t len);
int yz_stage_fd(const char *path, const char *name,
		struct ksu_file_load_policy *policy_state);
int yz_stage_file_fd(const char *path,
		     struct ksu_file_load_policy *policy_state);
void yz_early_packet_state_init(struct yz_early_packet_state *state);
void yz_close_early_packet_state(struct yz_early_packet_state *state);
int yz_stage_early_native_packet(u8 target_type, const char *target,
				 bool compat,
				 struct yz_early_packet_state *state);
void yz_schedule_injection(bool native, u8 target_type, bool early_native,
			   const char *label);

#endif
