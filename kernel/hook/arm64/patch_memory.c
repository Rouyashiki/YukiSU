/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 bmax121. All Rights Reserved.
 */

#ifndef __aarch64__
#error "YukiSU supports ARM64 kernels only"
#endif // #ifndef __aarch64__

#include "hook/patch_memory.h"
#include "klog.h" // IWYU pragma: keep
#include <linux/cpumask.h>
#include <linux/gfp.h> // IWYU pragma: keep
#include <linux/stop_machine.h>
#include <linux/uaccess.h>
#include <asm/cacheflush.h>
#include <asm-generic/fixmap.h>

unsigned long phys_from_virt(unsigned long addr, int *err)
{
	struct mm_struct *mm = &init_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	*err = 0;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		goto fail;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		goto fail;
#if defined(p4d_leaf)
	if (p4d_leaf(*p4d)) {
		return __p4d_to_phys(*p4d) + ((addr & ~P4D_MASK));
	}
#endif // #if defined(p4d_leaf)

	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		goto fail;
#if defined(pud_leaf)
	if (pud_leaf(*pud)) {
		return __pud_to_phys(*pud) + ((addr & ~PUD_MASK));
	}
#endif // #if defined(pud_leaf)

	pmd = pmd_offset(pud, addr);
#if defined(pmd_leaf)
	if (pmd_leaf(*pmd)) {
		return __pmd_to_phys(*pmd) + ((addr & ~PMD_MASK));
	}
#endif // #if defined(pmd_leaf)

	if (pmd_none(*pmd) || pmd_bad(*pmd))
		goto fail;

	pte = pte_offset_kernel(pmd, addr);
	if (!pte)
		goto fail;
	if (!pte_present(*pte))
		goto fail;

	return __pte_to_phys(*pte) + ((addr & ~PAGE_MASK));

fail:
	*err = -ENOENT;
	return 0;
}

#ifdef KSU_HAS_NEW_DCACHE_FLUSH
#define ksu_flush_dcache(start, sz)                                            \
	({                                                                     \
		unsigned long __start = (start);                               \
		unsigned long __end = __start + (sz);                          \
		dcache_clean_inval_poc(__start, __end);                        \
	})
#define ksu_flush_icache(start, end) caches_clean_inval_pou(start, end)
#else
#define ksu_flush_dcache(start, sz) __flush_dcache_area((void *)start, sz)
#define ksu_flush_icache(start, end) __flush_icache_range(start, end)
#endif // #ifdef KSU_HAS_NEW_DCACHE_FLUSH

struct patch_text_info {
	void *dst;
	void *src;
	size_t len;
	atomic_t cpu_count;
	int flags;
};

// Runs under stop_machine; propagate errors without console output.
static int ksu_patch_text_nosync(void *dst, void *src, size_t len, int flags)
{
	unsigned long p = (unsigned long)dst;
	unsigned long phy;
	void *map;
	int phy_err;
	int ret;

	phy = phys_from_virt(p, &phy_err);
	if (phy_err) {
		ret = phy_err;
		goto err;
	}

	map = (void *)set_fixmap_offset(FIX_TEXT_POKE0, phy);

	ret = (int)copy_to_kernel_nofault(map, src, len);

	clear_fixmap(FIX_TEXT_POKE0);

	if (!ret) {
		if (flags & KSU_PATCH_TEXT_FLUSH_ICACHE)
			ksu_flush_icache((uintptr_t)dst, (uintptr_t)dst + len);
		if (flags & KSU_PATCH_TEXT_FLUSH_DCACHE)
			ksu_flush_dcache((unsigned long)dst, len);
	}

err:
	return ret;
}

static int ksu_patch_text_cb(void *arg)
{
	struct patch_text_info *pp = arg;
	int ret = 0;

	if (atomic_inc_return(&pp->cpu_count) == num_online_cpus()) {
		ret =
		    ksu_patch_text_nosync(pp->dst, pp->src, pp->len, pp->flags);
		atomic_inc(&pp->cpu_count);
	} else {
		while (atomic_read(&pp->cpu_count) <= num_online_cpus())
			cpu_relax();
		isb();
	}

	return ret;
}

int ksu_patch_text(void *dst, void *src, size_t len, int flags)
{
	int ret;
	struct patch_text_info info = {
	    .dst = dst,
	    .src = src,
	    .len = len,
	    .cpu_count = ATOMIC_INIT(0),
	    .flags = flags,
	};

	ret = stop_machine(ksu_patch_text_cb, &info, cpu_online_mask);
	if (ret)
		pr_err("failed to patch kernel address 0x%lx: %d\n",
		       (unsigned long)dst, ret);
	return ret;
}

/*
 * Scan the memory region [start, start+size) for a BL instruction whose
 * branch target equals `target`. Returns the address of the first matching
 * instruction, or NULL if none is found.
 *
 * AArch64 BL encoding: bits[31:26] = 0b100101, bits[25:0] = imm26.
 * Branch target = PC + SignExtend(imm26, 26) * 4.
 */
void *scan_call_to(void *start, size_t size, void *target)
{
	const uint32_t *insn = (const uint32_t *)start;
	size_t count = size / sizeof(uint32_t);
	size_t i;

	for (i = 0; i < count; i++) {
		int32_t imm26;
		void *branch_target;

		/* Check BL opcode: bits[31:26] == 0b100101 */
		if ((insn[i] & 0xFC000000U) != 0x94000000U)
			continue;

		/* Sign-extend the 26-bit immediate to 32 bits */
		imm26 = (int32_t)((insn[i] & 0x03FFFFFFU) << 6) >> 6;

		/* Branch target = PC + imm26 * 4 */
		branch_target =
		    (void *)((uintptr_t)(&insn[i]) + (int64_t)imm26 * 4);

		if (branch_target == target)
			return (void *)&insn[i];
	}

	return NULL;
}
