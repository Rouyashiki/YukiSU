#include <linux/elf.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/path.h>
#include <linux/pid_namespace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>

#include "hook/lsm_hook.h"
#include "tango.h"
#include "klog.h"

#define YZ_TANGO_PHNUM 32
#define YZ_TANGO_STUB_SIZE 176
#define YZ_TANGO_SLOTS 16
#define YZ_R_ARM_JUMP_SLOT 22

struct yz_tango_image {
	unsigned long bias;
	Elf32_Ehdr ehdr;
	Elf32_Phdr phdr[YZ_TANGO_PHNUM];
};

struct yz_tango_work {
	struct callback_head cb;
	struct mm_struct *mm;
	unsigned long relro_start;
	unsigned long relro_end;
	struct yz_tango_image app;
	struct yz_tango_image linker;
};

struct yz_tango_slot {
	struct mm_struct *mm;
	u64 start;
	pid_t pid;
};

static struct yz_tango_slot yz_tango_slots[YZ_TANGO_SLOTS];
static DEFINE_SPINLOCK(yz_tango_lock);
static bool yz_tango_registered;

static bool yz_tango_leaf(struct file *file, const char *name)
{
	return file && !strcmp(file->f_path.dentry->d_name.name, name);
}

bool yz_tango_is_process(void)
{
	char path[96];
	char *exe;

	if (!current->mm ||
	    !yz_tango_leaf(current->mm->exe_file, "tango_translator"))
		return false;
	exe = d_path(&current->mm->exe_file->f_path, path, sizeof(path));
	return !IS_ERR(exe) && !strcmp(exe, "/system_ext/bin/tango_translator");
}

static bool yz_tango_range(const struct yz_tango_image *image,
			   unsigned long addr, size_t size, u32 flags)
{
	u16 i;

	if (addr > U32_MAX || size > U32_MAX - addr)
		return false;
	for (i = 0; i < image->ehdr.e_phnum; i++) {
		const Elf32_Phdr *ph = &image->phdr[i];
		unsigned long start = image->bias + ph->p_vaddr;

		if (ph->p_type == PT_LOAD && (ph->p_flags & flags) == flags &&
		    addr >= start && addr - start <= ph->p_memsz &&
		    size <= ph->p_memsz - (addr - start))
			return true;
	}
	return false;
}

static bool yz_tango_read(const struct yz_tango_image *image,
			  unsigned long addr, void *buf, size_t size)
{
	return yz_tango_range(image, addr, size, PF_R) &&
	       !copy_from_user(buf, (const void __user *)addr, size);
}

static bool yz_tango_image_init(struct yz_tango_image *image,
				unsigned long addr)
{
	Elf32_Ehdr *eh = &image->ehdr;
	bool found = false;
	u16 i;

	if (!addr || addr > U32_MAX - sizeof(*eh) ||
	    copy_from_user(eh, (const void __user *)addr, sizeof(*eh)) ||
	    memcmp(eh->e_ident, ELFMAG, SELFMAG) ||
	    eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_ARM ||
	    eh->e_type != ET_DYN || eh->e_phentsize != sizeof(Elf32_Phdr) ||
	    !eh->e_phnum || eh->e_phnum > YZ_TANGO_PHNUM ||
	    eh->e_phoff > PAGE_SIZE - eh->e_phnum * sizeof(Elf32_Phdr) ||
	    addr > U32_MAX - PAGE_SIZE ||
	    copy_from_user(image->phdr,
			   (const void __user *)(addr + eh->e_phoff),
			   eh->e_phnum * sizeof(Elf32_Phdr)))
		return false;

	for (i = 0; i < eh->e_phnum; i++) {
		const Elf32_Phdr *ph = &image->phdr[i];

		if (ph->p_type != PT_LOAD || ph->p_offset != 0)
			continue;
		if (addr < ph->p_vaddr || ph->p_vaddr & ~PAGE_MASK)
			return false;
		image->bias = addr - ph->p_vaddr;
		found = true;
		break;
	}
	if (!found)
		return false;
	for (i = 0; i < eh->e_phnum; i++) {
		const Elf32_Phdr *ph = &image->phdr[i];

		if (ph->p_type == PT_LOAD &&
		    (ph->p_filesz > ph->p_memsz ||
		     image->bias + ph->p_vaddr > U32_MAX ||
		     ph->p_memsz > U32_MAX - image->bias - ph->p_vaddr))
			return false;
	}
	return yz_tango_range(image, addr, sizeof(*eh), PF_R);
}

static u32 yz_tango_libc_got(const struct yz_tango_image *image)
{
	unsigned long symtab = 0, strtab = 0, rel = 0;
	u32 strsz = 0, relsz = 0, reltype = 0, syment = 0;
	u16 i;
	u32 n;

	for (i = 0; i < image->ehdr.e_phnum; i++) {
		const Elf32_Phdr *ph = &image->phdr[i];

		if (ph->p_type != PT_DYNAMIC || ph->p_memsz > 65536)
			continue;
		for (n = 0; n < ph->p_memsz / sizeof(Elf32_Dyn); n++) {
			Elf32_Dyn dyn;
			unsigned long addr =
			    image->bias + ph->p_vaddr + n * sizeof(dyn);

			if (!yz_tango_read(image, addr, &dyn, sizeof(dyn)))
				return 0;
			if (dyn.d_tag == DT_NULL)
				break;
			switch (dyn.d_tag) {
			case DT_SYMTAB:
				symtab = image->bias + dyn.d_un.d_ptr;
				break;
			case DT_STRTAB:
				strtab = image->bias + dyn.d_un.d_ptr;
				break;
			case DT_STRSZ:
				strsz = dyn.d_un.d_val;
				break;
			case DT_SYMENT:
				syment = dyn.d_un.d_val;
				break;
			case DT_JMPREL:
				rel = image->bias + dyn.d_un.d_ptr;
				break;
			case DT_PLTRELSZ:
				relsz = dyn.d_un.d_val;
				break;
			case DT_PLTREL:
				reltype = dyn.d_un.d_val;
				break;
			}
		}
		break;
	}
	if (!symtab || !strtab || !rel || syment != sizeof(Elf32_Sym) ||
	    !relsz || relsz > 65536 ||
	    (reltype != DT_REL && reltype != DT_RELA) ||
	    !yz_tango_range(image, strtab, strsz, PF_R) ||
	    !yz_tango_range(image, rel, relsz, PF_R))
		return 0;
	{
		u32 entsize =
		    reltype == DT_REL ? sizeof(Elf32_Rel) : sizeof(Elf32_Rela);

		if (relsz % entsize)
			return 0;
		for (n = 0; n < relsz / entsize; n++) {
			Elf32_Rel r;
			Elf32_Sym sym;
			char name[sizeof("__libc_init")];
			unsigned long got;

			if (!yz_tango_read(image, rel + n * entsize, &r,
					   sizeof(r)))
				return 0;
			if (ELF32_R_TYPE(r.r_info) != YZ_R_ARM_JUMP_SLOT)
				continue;
			if (!yz_tango_read(image,
					   symtab + ELF32_R_SYM(r.r_info) *
							sizeof(sym),
					   &sym, sizeof(sym)) ||
			    sym.st_name > strsz ||
			    sizeof(name) > strsz - sym.st_name ||
			    !yz_tango_read(image, strtab + sym.st_name, name,
					   sizeof(name)) ||
			    memcmp(name, "__libc_init", sizeof(name)))
				continue;
			got = image->bias + r.r_offset;
			if (!(got & 3) &&
			    yz_tango_range(image, got, sizeof(u32), PF_W))
				return got;
		}
	}
	return 0;
}

static bool yz_tango_exec_address(unsigned long addr, const char *leaf)
{
	struct vm_area_struct *vma;
	bool valid;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, addr);
	valid = vma && vma->vm_start <= addr && (vma->vm_flags & VM_EXEC) &&
		!(vma->vm_flags & VM_SHARED) &&
		yz_tango_leaf(vma->vm_file, leaf);
	mmap_read_unlock(current->mm);
	return valid;
}

static u32 yz_tango_padding(const struct yz_tango_image *image,
			    const char *leaf)
{
	u8 bytes[YZ_TANGO_STUB_SIZE];
	u16 i, j;

	for (i = 0; i < image->ehdr.e_phnum; i++) {
		const Elf32_Phdr *ph = &image->phdr[i];
		unsigned long end, stub, limit;
		bool overlap = false;

		if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
			continue;
		end = image->bias + ph->p_vaddr + ph->p_memsz;
		stub = ALIGN(end, 8);
		limit = PAGE_ALIGN(end);
		if (limit > U32_MAX || stub > limit ||
		    YZ_TANGO_STUB_SIZE > limit - stub)
			continue;
		for (j = 0; j < image->ehdr.e_phnum; j++) {
			const Elf32_Phdr *other = &image->phdr[j];
			unsigned long start = image->bias + other->p_vaddr;

			if (j != i && other->p_type == PT_LOAD &&
			    start < limit && start + other->p_memsz > stub)
				overlap = true;
		}
		if (overlap)
			continue;
		for (; sizeof(bytes) <= limit - stub; stub += 8) {
			if (!yz_tango_exec_address(stub, leaf) ||
			    !yz_tango_exec_address(stub + sizeof(bytes) - 1,
						   leaf) ||
			    copy_from_user(bytes, (const void __user *)stub,
					   sizeof(bytes)))
				break;
			if (!memchr_inv(bytes, 0, sizeof(bytes)))
				return stub;
		}
	}
	return 0;
}

static void yz_tango_bootstrap(u8 *bytes, u32 got, u32 original, u32 stub,
			       u32 dlopen, u32 dlsym, int fd)
{
	/* Thumb: save r0-r12/lr, dlopen(fd), close(fd), dlsym, call, restore,
	 * then tail-call __libc_init without rewinding the translator state. */
	/* Bionic LP32: RTLD_NOW=0; 2 is RTLD_GLOBAL and blocks unloading. */
	static const u8 code[64] = {
	    0x2d, 0xe9, 0xff, 0x5f, 0x0f, 0xf2, 0x38, 0x04, 0x04, 0xf1, 0x48,
	    0x00, 0x00, 0x21, 0x04, 0xf1, 0x20, 0x02, 0x63, 0x68, 0x25, 0x69,
	    0xa8, 0x47, 0x06, 0x46, 0xa0, 0x69, 0x06, 0x27, 0x00, 0xdf, 0x56,
	    0xb1, 0x30, 0x46, 0x04, 0xf1, 0x58, 0x01, 0x62, 0x68, 0x65, 0x69,
	    0xa8, 0x47, 0x18, 0xb1, 0x05, 0x46, 0x94, 0xe8, 0x0f, 0x00, 0xa8,
	    0x47, 0xbd, 0xe8, 0xff, 0x5f, 0xdf, 0xf8, 0x04, 0xf0,
	};
	u32 data[] = {got,    original, stub, YZ_TANGO_STUB_SIZE,
		      dlopen, dlsym,	fd,   0};
	u64 flags = 0x10; /* ANDROID_DLEXT_USE_LIBRARY_FD */

	memset(bytes, 0, YZ_TANGO_STUB_SIZE);
	memcpy(bytes, code, sizeof(code));
	memcpy(bytes + 64, data, sizeof(data));
	memcpy(bytes + 96, &flags, sizeof(flags));
	memcpy(bytes + 116, &fd, sizeof(fd));
	memcpy(bytes + 136, "libzygisk32.so", sizeof("libzygisk32.so"));
	memcpy(bytes + 152, "zygisk_core_entry_tango",
	       sizeof("zygisk_core_entry_tango"));
}

static bool yz_tango_claim(void)
{
	unsigned long flags;
	struct yz_tango_slot *slot;
	struct task_struct *task;
	int free_slot = -1;
	u32 i;
	bool claimed = false;

	rcu_read_lock();
	spin_lock_irqsave(&yz_tango_lock, flags);
	for (i = 0; i < YZ_TANGO_SLOTS; i++) {
		slot = &yz_tango_slots[i];
		if (slot->mm == current->mm && slot->pid == current->tgid &&
		    slot->start == current->start_boottime)
			goto out;
		task = slot->pid
			   ? pid_task(find_pid_ns(slot->pid, &init_pid_ns),
				      PIDTYPE_PID)
			   : NULL;
		if (free_slot < 0 && (!task || READ_ONCE(task->exit_state) ||
				      task->start_boottime != slot->start ||
				      READ_ONCE(task->mm) != slot->mm))
			free_slot = i;
	}
	if (free_slot < 0)
		goto out;
	slot = &yz_tango_slots[free_slot];
	slot->mm = current->mm;
	slot->pid = current->tgid;
	slot->start = current->start_boottime;
	claimed = true;
out:
	spin_unlock_irqrestore(&yz_tango_lock, flags);
	rcu_read_unlock();
	return claimed;
}

static void yz_tango_inject(struct callback_head *cb)
{
	struct yz_tango_work *work = container_of(cb, struct yz_tango_work, cb);
	struct vm_area_struct *vma;
	unsigned long app = 0, linker = 0;
	u64 dlopen_off, dlsym_off;
	u32 got, original, stub, redirect, generation = 0;
	u8 bytes[YZ_TANGO_STUB_SIZE];
	int fd, n;
	u32 count = 0;
	const char *stage = "images";
	bool redirected = false;

	if (!yz_tango_active() || current->mm != work->mm ||
	    current->flags & PF_EXITING)
		goto out;
	mmap_read_lock(current->mm);
	for (vma = find_vma(current->mm, 0); vma && vma->vm_start <= U32_MAX;
	     vma = find_vma(current->mm, vma->vm_end)) {
		count++;
		if (vma->vm_pgoff || !(vma->vm_flags & VM_READ))
			continue;
		if (yz_tango_leaf(vma->vm_file, "app_process32"))
			app = vma->vm_start;
		else if (yz_tango_leaf(vma->vm_file, "linker"))
			linker = vma->vm_start;
	}
	mmap_read_unlock(current->mm);
	if (!yz_tango_image_init(&work->app, app) ||
	    !yz_tango_image_init(&work->linker, linker))
		goto skipped;
	stage = "libc-got";
	got = yz_tango_libc_got(&work->app);
	if (!got || got < work->relro_start ||
	    (unsigned long)got + sizeof(original) > work->relro_end ||
	    get_user(original, (const u32 __user *)(unsigned long)got) ||
	    !yz_tango_exec_address(original & ~1UL, "libc.so"))
		goto skipped;
	if (!yz_tango_claim())
		goto out;
	fd = yz_tango_prepare(&generation);
	if (fd < 0) {
		pr_info("yukizygisk: Tango prepare failed pid=%d err=%d\n",
			current->tgid, fd);
		goto out;
	}
	stub = yz_tango_padding(&work->app, "app_process32");
	if (!stub)
		stub = yz_tango_padding(&work->linker, "linker");
	yz_tango_linker_offsets(&dlopen_off, &dlsym_off);
	if (!stub || !dlopen_off || !dlsym_off ||
	    dlopen_off > U32_MAX - work->linker.bias ||
	    dlsym_off > U32_MAX - work->linker.bias ||
	    !yz_tango_range(&work->linker,
			    (work->linker.bias + dlopen_off) & ~1UL, 2, PF_X) ||
	    !yz_tango_range(&work->linker,
			    (work->linker.bias + dlsym_off) & ~1UL, 2, PF_X)) {
		pr_warn(
		    "yukizygisk: Tango bootstrap unavailable pid=%d stub=%x\n",
		    current->tgid, stub);
		goto finish;
	}
	yz_tango_bootstrap(bytes, got, original, stub,
			   work->linker.bias + dlopen_off,
			   work->linker.bias + dlsym_off, fd);
	n = access_process_vm(current, stub, bytes, sizeof(bytes),
			      FOLL_FORCE | FOLL_WRITE);
	if (n != sizeof(bytes))
		goto restore;
	flush_icache_range(stub, stub + sizeof(bytes));
	redirect = stub | 1;
	n = access_process_vm(current, got, &redirect, sizeof(redirect),
			      FOLL_FORCE | FOLL_WRITE);
	if (n == sizeof(redirect)) {
		redirected = true;
		goto finish;
	}
	/* Keep the trampoline and FD alive if rollback cannot be confirmed. */
	if (access_process_vm(current, got, &original, sizeof(original),
			      FOLL_FORCE | FOLL_WRITE) != sizeof(original)) {
		redirected = true;
		pr_err("yukizygisk: Tango GOT rollback failed pid=%d\n",
		       current->tgid);
		goto finish;
	}
restore:
	memset(bytes, 0, sizeof(bytes));
	access_process_vm(current, stub, bytes, sizeof(bytes),
			  FOLL_FORCE | FOLL_WRITE);
finish:
	pr_info("yukizygisk: Tango guest bootstrap pid=%d got=%x stub=%x "
		"redirected=%d\n",
		current->tgid, got, stub, redirected);
	yz_tango_finish(generation, fd, redirected);
	goto out;
skipped:
	pr_info("yukizygisk: Tango guest skipped pid=%d stage=%s maps=%u "
		"app=%lx linker=%lx relro=%lx-%lx\n",
		current->tgid, stage, count, app, linker, work->relro_start,
		work->relro_end);
out:
	kfree(work);
	module_put(THIS_MODULE);
}

static int yz_tango_mprotect(struct vm_area_struct *vma, unsigned long reqprot,
			     unsigned long prot);
static struct ksu_lsm_hook yz_tango_hook = KSU_LSM_HOOK_INIT(
    file_mprotect, "selinux_file_mprotect", yz_tango_mprotect, 0);

static int yz_tango_mprotect(struct vm_area_struct *vma, unsigned long reqprot,
			     unsigned long prot)
{
	typeof(((union security_list_options *)0)->file_mprotect) original =
	    READ_ONCE(yz_tango_hook.original);
	struct yz_tango_work *work;
	int ret = original(vma, reqprot, prot);

	if (ret || !yz_tango_active() || !current->mm ||
	    current->pid != current->tgid || !(vma->vm_flags & VM_WRITE) ||
	    vma->vm_flags & VM_SHARED || prot != PROT_READ ||
	    vma->vm_start > U32_MAX ||
	    !yz_tango_leaf(vma->vm_file, "app_process32") ||
	    !yz_tango_is_process())
		return ret;
	pr_info("yukizygisk: Tango guest RELRO pid=%d range=%lx-%lx\n",
		current->tgid, vma->vm_start, vma->vm_end);
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return ret;
	if (!try_module_get(THIS_MODULE)) {
		kfree(work);
		return ret;
	}
	work->mm = current->mm;
	work->relro_start = vma->vm_start;
	work->relro_end = vma->vm_end;
	init_task_work(&work->cb, yz_tango_inject);
	if (task_work_add(current, &work->cb, TWA_RESUME)) {
		kfree(work);
		module_put(THIS_MODULE);
	}
	return ret;
}

int yz_tango_enable(void)
{
	int ret;

	if (yz_tango_registered)
		return 0;
	ret = ksu_register_lsm_hook(&yz_tango_hook);
	if (!ret)
		yz_tango_registered = true;
	return ret;
}

void yz_tango_disable(void)
{
	if (yz_tango_registered) {
		ksu_unregister_lsm_hook(&yz_tango_hook);
		yz_tango_registered = false;
	}
}
