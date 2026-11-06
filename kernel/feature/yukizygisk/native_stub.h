#ifndef YZ_NATIVE_STUB_H
#define YZ_NATIVE_STUB_H

#define YZ_NATIVE_STUB_DATA 0x400
#define YZ_NATIVE_STUB_MAGIC 0x594e4231
#define YZ_NATIVE_STUB_VERSION 1
#define YZ_NS_ENTRY 8
#define YZ_NS_LINKER 16
#define YZ_NS_RESOLVER 24
#define YZ_NS_LENGTH 32
#define YZ_NS_BOOTSTRAP 40
#define YZ_NS_LOADER 48
#define YZ_NS_PID 56
#define YZ_NS_GENERATION 60
#define YZ_NS_LOADER_FD 64
#define YZ_NS_CORE_FD 68
#define YZ_NS_PACKET 72
#define YZ_NS_COUNT 76
#define YZ_NS_FDS 80
#define YZ_NS_STATUS 352
#define YZ_NS_MESSAGE 360
#define YZ_NS_REPORT_FD 616
#define YZ_NS_REPORT_SIZE 264
#define YZ_NS_COMPLETION 624
#define YZ_NS_ERROR 640
#define YZ_NS_OWNERSHIP 644
#define YZ_NS_CLOSE 648

#ifndef __ASSEMBLER__
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <linux/types.h>
#include <stddef.h>
#endif
/* Fixed offsets shared by the generated ARM and AArch64 launchers. All
 * pointers are process-local values, never packet-v3 wire fields. */
struct yz_native_stub {
	__u32 magic, version;
	__aligned_u64 entry, linker, resolver, resolver_size, bootstrap, loader;
	__u32 process, generation;
	__s32 loader_fd, core_fd, packet_argument;
	__u32 cleanup_count;
	__s32 cleanup_fds[68];
	__s32 status;
	__u32 stage;
	char message[256];
	__s32 report_fd;
	__u32 tail;
	struct {
		__u32 size, version, process, generation;
		__s32 error;
		__u32 ownership;
	} completion;
	__aligned_u64 close_loader;
	__aligned_u64 libc_reference;
};
#endif
#endif
