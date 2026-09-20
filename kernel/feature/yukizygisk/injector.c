#include <linux/auxvec.h>
#include <linux/compat.h>
#include <linux/elf.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

#include "internal.h"
#include "tango.h"
#include "selinux/selinux.h"
#include "klog.h" // IWYU pragma: keep
#include "uapi/yukizygisk.h"

#define YZ_STUB_EXTINFO_OFF 0xa00
#define YZ_STUB_STR_OFF 0xc00
#define YZ_STUB_ENTRY_STR_OFF 0xd00

/* Patch a movz/movk x<d> sequence. */
static void __maybe_unused yz_patch_imm64(u32 *insn, u64 val)
{
	int i;

	for (i = 0; i < 4; i++) {
		u16 imm = (val >> (16 * i)) & 0xffff;

		insn[i] = (insn[i] & ~(0xffffu << 5)) | ((u32)imm << 5);
	}
}

/* bionic android_dlextinfo, LP64 subset. */
struct yz_dlextinfo {
	__u64 flags;
	__u64 reserved_addr;
	__u64 reserved_size;
	__s32 relro_fd;
	__s32 library_fd;
	__s64 library_fd_offset;
	__u64 library_namespace;
};

/* bionic android_dlextinfo, ARM EABI subset. */
struct yz_compat_dlextinfo {
	__u64 flags;
	__u32 reserved_addr;
	__u32 reserved_size;
	__s32 relro_fd;
	__s32 library_fd;
	__s64 library_fd_offset;
	__u32 library_namespace;
};
static_assert(offsetof(struct yz_compat_dlextinfo, library_fd) == 20);
static_assert(sizeof(struct yz_compat_dlextinfo) == 40);

enum yz_inject_kind {
	YZ_INJECT_ZYGOTE = 1,
	YZ_INJECT_NATIVE = 2,
};

struct yz_inject_tw {
	struct callback_head cb;
	enum yz_inject_kind kind;
	u8 native_target_type;
	bool early_native;
	char label[YZ_NATIVE_TARGET_VALUE_MAX];
};

static int yz_find_stack_at_entry(struct pt_regs *regs, bool compat,
				  unsigned long *entry_addr,
				  unsigned long *entry_value)
{
	unsigned long p = user_stack_pointer(regs);
	unsigned long word;
	int argc;

#ifdef CONFIG_COMPAT
	if (compat) {
		compat_ulong_t cword;

		if (get_user(cword, (compat_ulong_t __user *)p))
			return -EFAULT;
		argc = (int)cword;
		p += sizeof(cword);
		p += (unsigned long)(argc + 1) * sizeof(cword);
		for (;;) {
			if (get_user(cword, (compat_ulong_t __user *)p))
				return -EFAULT;
			p += sizeof(cword);
			if (!cword)
				break;
		}
		for (;;) {
			compat_ulong_t cval;

			if (get_user(cword, (compat_ulong_t __user *)p) ||
			    get_user(
				cval,
				(compat_ulong_t __user *)(p + sizeof(cword))))
				return -EFAULT;
			if (cword == AT_NULL)
				return -ENOENT;
			if (cword == AT_ENTRY) {
				*entry_addr = p + sizeof(cword);
				*entry_value = cval;
				return 0;
			}
			p += 2 * sizeof(cword);
		}
	}
#else
	(void)compat;
#endif

	if (get_user(word, (unsigned long __user *)p))
		return -EFAULT;
	argc = (int)word;
	p += sizeof(word);
	p += (unsigned long)(argc + 1) * sizeof(word);
	for (;;) {
		if (get_user(word, (unsigned long __user *)p))
			return -EFAULT;
		p += sizeof(word);
		if (!word)
			break;
	}
	for (;;) {
		unsigned long val;

		if (get_user(word, (unsigned long __user *)p) ||
		    get_user(val, (unsigned long __user *)(p + sizeof(word))))
			return -EFAULT;
		if (word == AT_NULL)
			return -ENOENT;
		if (word == AT_ENTRY) {
			*entry_addr = p + sizeof(word);
			*entry_value = val;
			return 0;
		}
		p += 2 * sizeof(word);
	}
}

static int yz_write_stack_at_entry(unsigned long addr, unsigned long value,
				   bool compat)
{
#ifdef CONFIG_COMPAT
	if (compat)
		return put_user((compat_ulong_t)value,
				(compat_ulong_t __user *)addr);
#else
	(void)compat;
#endif
	return put_user(value, (unsigned long __user *)addr);
}

static int yz_read_stack_at_entry(unsigned long addr, unsigned long *value,
				  bool compat)
{
#ifdef CONFIG_COMPAT
	if (compat) {
		compat_ulong_t cvalue;
		int ret = get_user(cvalue, (compat_ulong_t __user *)addr);

		*value = cvalue;
		return ret;
	}
#else
	(void)compat;
#endif
	return get_user(*value, (unsigned long __user *)addr);
}

static void yz_read_saved_auxv(struct mm_struct *mm, bool compat,
			       unsigned long *entry, unsigned long *base)
{
	size_t k;

	*entry = 0;
	*base = 0;
#ifdef CONFIG_COMPAT
	if (compat) {
		const compat_ulong_t *auxv =
		    (const compat_ulong_t *)mm->saved_auxv;
		const size_t auxv_bytes = sizeof(mm->saved_auxv);
		const size_t words = auxv_bytes / sizeof(*auxv);

		for (k = 0; k + 1 < words; k += 2) {
			compat_ulong_t type = auxv[k];

			if (type == AT_NULL)
				break;
			if (type == AT_ENTRY)
				*entry = auxv[k + 1];
			else if (type == AT_BASE)
				*base = auxv[k + 1];
		}
		return;
	}
#else
	(void)compat;
#endif

	for (k = 0; k + 1 < AT_VECTOR_SIZE; k += 2) {
		unsigned long type = mm->saved_auxv[k];

		if (type == AT_NULL)
			break;
		if (type == AT_ENTRY)
			*entry = mm->saved_auxv[k + 1];
		else if (type == AT_BASE)
			*base = mm->saved_auxv[k + 1];
	}
}

/* Runs in target context to stage the loader and redirect AT_ENTRY. */
static void yz_inject_tw_func(struct callback_head *cb)
{
	struct yz_inject_tw *tw = container_of(cb, struct yz_inject_tw, cb);
	struct mm_struct *mm = current->mm;
	struct pt_regs *uregs;
	unsigned long saved = 0, at_entry_uaddr = 0, at_entry_uval = 0;
	unsigned long at_base = 0;
	u64 dlopen_off, dlsym_off;
	u64 start_boottime;
	u32 runtime_generation = 0;
	char process[YZ_RUNTIME_PROCESS_MAX];
	char socket_name[YZ_NATIVE_TARGET_VALUE_MAX];
	bool native = tw->kind == YZ_INJECT_NATIVE;
	bool compat = false;
	bool runtime_redirected = false;
	bool runtime_safemode = false;
	u32 runtime_flags =
	    native && tw->early_native ? YZ_RUNTIME_F_EARLY_NATIVE : 0;
	u8 runtime_abi;
	u8 runtime_kind =
	    native ? YZ_RUNTIME_KIND_NATIVE : YZ_RUNTIME_KIND_ZYGOTE;

	if (!READ_ONCE(yukizygisk_enabled) &&
	    !(native && tw->early_native && yz_early_native_active()))
		goto out;
	if (!native && yz_tango_is_process()) {
		pr_info("yukizygisk: Tango host exec pid=%d; waiting for guest "
			"RELRO\n",
			current->tgid);
		goto out;
	}

#ifdef CONFIG_COMPAT
	compat = is_compat_task();
#endif
	runtime_abi = compat ? YZ_RUNTIME_ABI_32 : YZ_RUNTIME_ABI_64;
	start_boottime = READ_ONCE(current->start_boottime);
	yz_runtime_read_process(mm, process, sizeof(process));
	if (native) {
		yz_copy_name(socket_name, sizeof(socket_name),
			     tw->label[0] ? tw->label : "native");
		runtime_generation = yz_runtime_begin(
		    runtime_kind, runtime_abi, tw->native_target_type,
		    runtime_flags, process, socket_name, start_boottime);
		if (yz_safemode_is_active()) {
			pr_info("yukizygisk: safe mode skipped native target "
				"pid=%d target=%s\n",
				current->pid, socket_name);
			runtime_safemode = true;
			yz_runtime_set_state((u32)current->tgid,
					     runtime_generation,
					     YZ_RUNTIME_STATE_SAFEMODE);
			goto out;
		}
	} else {
		if (!mm ||
		    !yz_parse_zygote_args(mm, socket_name, YZ_ZYGOTE_NAME_MAX))
			goto out;
		runtime_generation =
		    yz_runtime_begin(runtime_kind, runtime_abi, 0, 0, process,
				     socket_name, start_boottime);
	}
	if (!mm)
		goto out;
	dlopen_off = compat ? yz_dlopen32_off : yz_dlopen_off;
	dlsym_off = compat ? yz_dlsym32_off : yz_dlsym_off;

	if (!native && yz_zygote_safemode_should_skip(socket_name)) {
		runtime_safemode = true;
		yz_runtime_set_state((u32)current->tgid, runtime_generation,
				     YZ_RUNTIME_STATE_SAFEMODE);
		goto out;
	}

	yz_read_saved_auxv(mm, compat, &saved, &at_base);

	uregs = task_pt_regs(current);
	if (yz_find_stack_at_entry(uregs, compat, &at_entry_uaddr,
				   &at_entry_uval))
		goto out;

	pr_info("yukizygisk: entry check pid=%d target=%s saved=0x%lx "
		"stack=0x%lx value=0x%lx result=%s\n",
		current->pid, socket_name, saved, at_entry_uaddr, at_entry_uval,
		(at_entry_uaddr && at_entry_uval == saved) ? "match"
							   : "mismatch");

	if (at_base && dlopen_off)
		pr_info("yukizygisk: linker resolved pid=%d target=%s "
			"base=0x%lx offset=0x%llx dlopen=0x%llx\n",
			current->pid, socket_name, at_base, dlopen_off,
			(u64)at_base + dlopen_off);

	if (at_entry_uaddr && at_entry_uval == saved) {
		unsigned long check = ~saved;
		int werr =
		    yz_write_stack_at_entry(at_entry_uaddr, saved, compat);
		int rerr =
		    yz_read_stack_at_entry(at_entry_uaddr, &check, compat);

		pr_info("yukizygisk: entry verification pid=%d target=%s "
			"address=0x%lx value=0x%lx write=%d read=%d "
			"readback=0x%lx result=%s\n",
			current->pid, socket_name, at_entry_uaddr, saved, werr,
			rerr, check,
			(!werr && !rerr && check == saved) ? "ok" : "failed");
	}

	/* Prepare the loader before replacing AT_ENTRY. */
	if ((READ_ONCE(yukizygisk_enabled) ||
	     (native && tw->early_native && yz_early_native_active())) &&
	    at_entry_uaddr && at_entry_uval == saved && saved) {
		/* Trampolines load the staged library, then resume AT_ENTRY. */
		static const u32 tmpl[] = {
		    0x10000013, 0xd10103ff, 0xd2800014, 0xf2a00014, 0xf2c00014,
		    0xf2e00014, 0xd2800015, 0xf2a00015, 0xf2c00015, 0xf2e00015,
		    0xd2800017, 0xf2a00017, 0xf2c00017, 0xf2e00017, 0xf90003f3,
		    0xf90007f4, 0xf9000bf7, 0xf9000fe0, 0x5289c430, 0x72aa4830,
		    0xb9080270, 0x91300260, 0xd2800041, 0x91280262, 0xaa1403e3,
		    0xd63f02a0, 0xf94003f3, 0xf9040660, 0xf90013e0, 0xb94a1e60,
		    0xd2800728, 0xd4000001, 0xf94003f3, 0xf94013e0, 0xb40000c0,
		    0x91340261, 0xf94007e2, 0xf9400bf7, 0xd63f02e0, 0xb50000a0,
		    0xd2800000, 0xd2800728, 0xd4000001, 0x14000005, 0xaa0003f9,
		    0xd2800000, 0xd2800001, 0xd63f0320, 0xf9400fe0, 0xf94007f4,
		    0x910103ff, 0xaa1403f0, 0xd61f0200,
		};
		static const u8 compat_tmpl[] = {
		    0x2d, 0xe9, 0xf7, 0x43, 0xf8, 0x46, 0xa8, 0xf1, 0x08, 0x08,
		    0x11, 0x4c, 0x11, 0x4d, 0x12, 0x4e, 0x08, 0xf6, 0x00, 0x40,
		    0x02, 0x21, 0x08, 0xf6, 0x00, 0x22, 0x23, 0x46, 0xa8, 0x47,
		    0x81, 0x46, 0x0e, 0x48, 0x06, 0x27, 0x00, 0xdf, 0x48, 0x46,
		    0x48, 0xb1, 0x08, 0xf6, 0x00, 0x51, 0x22, 0x46, 0xb0, 0x47,
		    0x20, 0xb1, 0x03, 0x46, 0x0a, 0x48, 0x0a, 0x49, 0x98, 0x47,
		    0x04, 0xe0, 0x08, 0x48, 0x00, 0x28, 0x01, 0xdb, 0x06, 0x27,
		    0x00, 0xdf, 0xa4, 0x46, 0xbd, 0xe8, 0xf7, 0x43, 0x60, 0x47,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00,
		};
		static_assert(sizeof(compat_tmpl) == 0x68);
		u32 code[ARRAY_SIZE(tmpl)];
		u8 compat_code[ARRAY_SIZE(compat_tmpl)];
		struct yz_dlextinfo extinfo;
		struct yz_compat_dlextinfo compat_extinfo;
		struct ksu_file_load_policy native_policy = {};
		struct yz_early_packet_state early_packet;
		unsigned long stub, dlopen_addr, dlsym_addr;
		unsigned long redirected_entry;
		int loader_fd, core_fd, stub_core_fd;
		int early_packet_arg, werr;
		bool copy_failed, yuki;
		const char *lib_str, *entry_str;
		const char *loader_path, *core_path;
		size_t lib_len, entry_len;
		char loader_name[YZ_VMA_NAME_LEN], core_name[YZ_VMA_NAME_LEN];

		yz_early_packet_state_init(&early_packet);
		if (!at_base || !dlopen_off || !dlsym_off) {
			pr_info("yukizygisk: linker addresses unavailable "
				"pid=%d target=%s; injection skipped\n",
				current->pid, socket_name);
			goto out;
		}
		dlopen_addr = at_base + dlopen_off;
		dlsym_addr = at_base + dlsym_off;

		yuki = native || yz_yukilinker_enabled;
		loader_path =
		    native && tw->early_native
			? yz_early_loader_path(compat)
			: (compat ? YZ_LOADER32_PATH : YZ_LOADER64_PATH);
		if (native)
			core_path = tw->early_native
					? yz_early_native_core_path(compat)
					: (compat ? YZ_NATIVE_CORE32_PATH
						  : YZ_NATIVE_CORE64_PATH);
		else
			core_path = compat ? YZ_CORE32_PATH : YZ_CORE64_PATH;
		yz_cache_name(loader_name, sizeof(loader_name));
		yz_cache_name(core_name, sizeof(core_name));
		if (yuki)
			loader_fd = yz_stage_fd(loader_path, loader_name,
						&native_policy);
		else if (native)
			loader_fd = yz_stage_file_fd(core_path, &native_policy);
		else
			loader_fd =
			    yz_stage_fd(core_path, core_name, &native_policy);
		if (loader_fd < 0) {
			pr_info("yukizygisk: loader staging failed pid=%d "
				"target=%s err=%d\n",
				current->pid, socket_name, loader_fd);
			goto out;
		}
		if (yuki) {
			core_fd = yz_stage_fd(core_path, core_name, NULL);
		} else {
			core_fd = loader_fd; /* dlopen the core directly */
		}
		if (yuki && core_fd < 0) {
			pr_info("yukizygisk: core staging failed pid=%d "
				"target=%s err=%d\n",
				current->pid, socket_name, core_fd);
			yz_close_current_fd(loader_fd);
			yz_restore_native_policy_state(&native_policy);
			goto out;
		}

		early_packet_arg = 0;
		if (native && tw->early_native) {
			int ret = yz_stage_early_native_packet(
			    tw->native_target_type, tw->label, compat,
			    &early_packet);

			if (ret < 0 || early_packet.packet_fd < 0 ||
			    early_packet.packet_fd >= 0xffff) {
				pr_info("yukizygisk: early native packet "
					"failed pid=%d target=%s err=%d\n",
					current->pid, socket_name, ret);
				yz_close_early_packet_state(&early_packet);
				yz_close_current_fd(loader_fd);
				if (yuki)
					yz_close_current_fd(core_fd);
				yz_restore_native_policy_state(&native_policy);
				goto out;
			}
			early_packet_arg = early_packet.packet_fd + 1;
		}
		{
			int ret = ksu_file_load_policy_allow_execmem_current(
			    &native_policy);

			if (ret < 0) {
				pr_info("yukizygisk: execmem policy update "
					"failed pid=%d target=%s err=%d\n",
					current->pid, socket_name, ret);
				yz_close_current_fd(loader_fd);
				if (yuki)
					yz_close_current_fd(core_fd);
				yz_close_early_packet_state(&early_packet);
				yz_restore_native_policy_state(&native_policy);
				goto out;
			}
		}

		stub = vm_mmap(NULL, 0, PAGE_SIZE,
			       PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(stub)) {
			pr_info("yukizygisk: trampoline mapping failed pid=%d "
				"target=%s err=%ld\n",
				current->pid, socket_name, (long)stub);
			yz_close_current_fd(loader_fd);
			if (yuki)
				yz_close_current_fd(core_fd);
			yz_close_early_packet_state(&early_packet);
			yz_restore_native_policy_state(&native_policy);
			goto out;
		}

		memcpy(code, tmpl, sizeof(code));
		yz_patch_imm64(&code[2], saved); /* x20 = real entry */
		yz_patch_imm64(&code[6], dlopen_addr); /* x21 = dlopen */
		yz_patch_imm64(&code[10], dlsym_addr); /* x23 = dlsym */
		stub_core_fd = yuki ? core_fd : -1;
		code[40] = 0xd2800000u | (((u32)stub_core_fd & 0xffff) << 5);
		code[45] = 0xd2800000u | (((u32)stub_core_fd & 0xffff) << 5);
		code[46] =
		    0xd2800001u | (((u32)early_packet_arg & 0xffff) << 5);
		memcpy(compat_code, compat_tmpl, sizeof(compat_code));
		{
			u32 value = (u32)saved;

			memcpy(&compat_code[0x50], &value, sizeof(value));
			value = (u32)dlopen_addr;
			memcpy(&compat_code[0x54], &value, sizeof(value));
			value = (u32)dlsym_addr;
			memcpy(&compat_code[0x58], &value, sizeof(value));
			value = (u32)loader_fd;
			memcpy(&compat_code[0x5c], &value, sizeof(value));
			value = (u32)stub_core_fd;
			memcpy(&compat_code[0x60], &value, sizeof(value));
			value = (u32)early_packet_arg;
			memcpy(&compat_code[0x64], &value, sizeof(value));
		}

		if (yuki) {
			lib_str = compat ? YZ_LOADER32_NAME : YZ_LOADER64_NAME;
			lib_len = compat ? sizeof(YZ_LOADER32_NAME)
					 : sizeof(YZ_LOADER64_NAME);
			entry_str = "yuki_bootstrap";
			entry_len = sizeof("yuki_bootstrap");
		} else {
			if (native) {
				lib_str = compat ? YZ_NATIVE_CORE32_NAME
						 : YZ_NATIVE_CORE64_NAME;
				lib_len = compat
					      ? sizeof(YZ_NATIVE_CORE32_NAME)
					      : sizeof(YZ_NATIVE_CORE64_NAME);
			} else {
				lib_str =
				    compat ? YZ_CORE32_NAME : YZ_CORE64_NAME;
				lib_len = compat ? sizeof(YZ_CORE32_NAME)
						 : sizeof(YZ_CORE64_NAME);
			}
			entry_str = "zygisk_core_entry_direct";
			entry_len = sizeof("zygisk_core_entry_direct");
		}

		memset(&extinfo, 0, sizeof(extinfo));
		extinfo.flags = YZ_DLEXT_USE_LIBRARY_FD |
				(native ? YZ_DLEXT_FORCE_LOAD : 0);
		extinfo.library_fd = loader_fd;
		memset(&compat_extinfo, 0, sizeof(compat_extinfo));
		compat_extinfo.flags = extinfo.flags;
		compat_extinfo.library_fd = loader_fd;

		if (compat) {
			copy_failed =
			    copy_to_user((void __user *)stub, compat_code,
					 sizeof(compat_code)) ||
			    copy_to_user(
				(void __user *)(stub + YZ_STUB_EXTINFO_OFF),
				&compat_extinfo, sizeof(compat_extinfo));
		} else {
			copy_failed =
			    copy_to_user((void __user *)stub, code,
					 sizeof(code)) ||
			    copy_to_user(
				(void __user *)(stub + YZ_STUB_EXTINFO_OFF),
				&extinfo, sizeof(extinfo));
		}
		if (copy_failed ||
		    copy_to_user((void __user *)(stub + YZ_STUB_STR_OFF),
				 lib_str, lib_len) ||
		    copy_to_user((void __user *)(stub + YZ_STUB_ENTRY_STR_OFF),
				 entry_str, entry_len)) {
			pr_info("yukizygisk: trampoline copy failed pid=%d "
				"target=%s\n",
				current->pid, socket_name);
			vm_munmap(stub, PAGE_SIZE);
			yz_close_current_fd(loader_fd);
			if (yuki)
				yz_close_current_fd(core_fd);
			yz_close_early_packet_state(&early_packet);
			yz_restore_native_policy_state(&native_policy);
			goto out;
		}
		{
			u32 marker = 0x52414e21u;

			if (copy_to_user((void __user *)(stub + 0x800), &marker,
					 sizeof(marker))) {
				vm_munmap(stub, PAGE_SIZE);
				yz_close_current_fd(loader_fd);
				if (yuki)
					yz_close_current_fd(core_fd);
				yz_close_early_packet_state(&early_packet);
				yz_restore_native_policy_state(&native_policy);
				goto out;
			}
		}

		flush_icache_range(
		    stub, stub + (compat ? sizeof(compat_code) : sizeof(code)));

		redirected_entry = compat ? stub | 1UL : stub;
		werr = yz_write_stack_at_entry(at_entry_uaddr, redirected_entry,
					       compat);
		pr_info("yukizygisk: entry redirect pid=%d target=%s abi=%s "
			"stub=0x%lx loader_fd=%d core_fd=%d dlopen=0x%lx "
			"dlsym=0x%lx entry=0x%lx result=%s\n",
			current->pid, socket_name, compat ? "arm" : "arm64",
			stub, loader_fd, core_fd, dlopen_addr, dlsym_addr,
			saved, werr ? "failed" : "redirected");
		if (werr) {
			vm_munmap(stub, PAGE_SIZE);
			yz_close_current_fd(loader_fd);
			if (yuki)
				yz_close_current_fd(core_fd);
			yz_close_early_packet_state(&early_packet);
			yz_restore_native_policy_state(&native_policy);
		} else {
			runtime_redirected = true;
			yz_runtime_set_state((u32)current->tgid,
					     runtime_generation,
					     YZ_RUNTIME_STATE_REDIRECTED);
			yz_publish_native_policy_state(current->tgid,
						       &native_policy);
		}
	}
out:
	if (runtime_generation && !runtime_redirected && !runtime_safemode)
		yz_runtime_set_state((u32)current->tgid, runtime_generation,
				     YZ_RUNTIME_STATE_FAILED);
	kfree(tw);
}

void yz_schedule_injection(bool native, u8 target_type, bool early_native,
			   const char *label)
{
	struct yz_inject_tw *tw;

	if (!READ_ONCE(yukizygisk_enabled) &&
	    !(native && early_native && yz_early_native_active()))
		return;
	tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
	if (!tw)
		return;

	tw->kind = native ? YZ_INJECT_NATIVE : YZ_INJECT_ZYGOTE;
	if (native) {
		tw->native_target_type = target_type;
		tw->early_native = early_native;
		yz_copy_name(tw->label, sizeof(tw->label), label);
	}
	init_task_work(&tw->cb, yz_inject_tw_func);
	if (task_work_add(current, &tw->cb, TWA_RESUME))
		kfree(tw);
}

bool yz_tango_active(void)
{
	return READ_ONCE(yukizygisk_enabled);
}

void yz_tango_linker_offsets(u64 *dlopen, u64 *dlsym)
{
	*dlopen = READ_ONCE(yz_dlopen32_off);
	*dlsym = READ_ONCE(yz_dlsym32_off);
}

int yz_tango_prepare(u32 *generation)
{
	struct ksu_file_load_policy policy = {};
	char socket_name[YZ_ZYGOTE_NAME_MAX];
	char process[YZ_RUNTIME_PROCESS_MAX];
	int fd;

	if (!yz_tango_active() ||
	    !yz_parse_zygote_args(current->mm, socket_name,
				  sizeof(socket_name)))
		return -EINVAL;
	yz_runtime_read_process(current->mm, process, sizeof(process));
	*generation = yz_runtime_begin(
	    YZ_RUNTIME_KIND_ZYGOTE, YZ_RUNTIME_ABI_32, 0, 0, process,
	    socket_name, READ_ONCE(current->start_boottime));
	if (yz_zygote_safemode_should_skip(socket_name)) {
		yz_runtime_set_state(current->tgid, *generation,
				     YZ_RUNTIME_STATE_SAFEMODE);
		return -ECANCELED;
	}
	fd = yz_stage_fd(YZ_CORE32_PATH, YZ_VMA_NAME, &policy);
	if (fd < 0) {
		yz_restore_native_policy_state(&policy);
		yz_runtime_set_state(current->tgid, *generation,
				     YZ_RUNTIME_STATE_FAILED);
		return fd;
	}
	if (ksu_file_load_policy_allow_execmem_current(&policy)) {
		yz_close_current_fd(fd);
		yz_restore_native_policy_state(&policy);
		yz_runtime_set_state(current->tgid, *generation,
				     YZ_RUNTIME_STATE_FAILED);
		return -EACCES;
	}
	yz_publish_native_policy_state(current->tgid, &policy);
	return fd;
}

void yz_tango_finish(u32 generation, int fd, bool redirected)
{
	if (!redirected) {
		yz_close_current_fd(fd);
		ksu_yukizygisk_restore_native_load_policy(current->tgid);
	}
	yz_runtime_set_state(current->tgid, generation,
			     redirected ? YZ_RUNTIME_STATE_REDIRECTED
					: YZ_RUNTIME_STATE_FAILED);
}
