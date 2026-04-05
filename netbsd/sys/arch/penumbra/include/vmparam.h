/*	$NetBSD$	*/

#ifndef _PENUMBRA_VMPARAM_H_
#define _PENUMBRA_VMPARAM_H_

/*
 * Penumbra virtual memory layout — 2G/2G user/kernel split.
 *
 * 32-bit VA space, software-managed 64-entry TLB (no KSEG bypass).
 * All kernel pages require TLB entries with G=1 (global).
 *
 * User VA:    0x0000_1000 – 0x7FFF_FFFF  (2 GB - 4K)
 * Kernel VA:  0x8000_0000 – 0xFFFF_FFFF  (2 GB)
 *
 * User layout is compact to optimize TLB usage:
 *   0x0000_0000  unmapped null guard page
 *   0x0000_1000  text / data / bss
 *                heap (grows up)
 *                (gap)
 *   USRSTACK     stack top at 64 MB (grows down)
 *                mmap region above 64 MB (for large programs)
 *   0x7FFF_FFFF  end of user VA
 *
 * Page table optimization: processes start with a flat single-level
 * page table covering 0–64 MB (16K entries, 64 KB).  If VA usage
 * grows beyond 64 MB, pmap promotes to a 2-level table.
 *
 * Kernel VA includes identity-mappable device MMIO near the top
 * (ROM at 0xFFFF_0000, UART at 0xFF00_0000 physical).
 */

/* Page size: 4 KB (matches Penumbra MMU hardware) */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1 << PAGE_SHIFT)
#define PAGE_MASK	(PAGE_SIZE - 1)

/* User virtual address range */
#define VM_MIN_ADDRESS		((vaddr_t) 0x00001000)
#define VM_MAXUSER_ADDRESS	((vaddr_t) 0x80000000)

/* Kernel virtual address range */
#define VM_MIN_KERNEL_ADDRESS	((vaddr_t) 0x80000000)
#define VM_MAX_KERNEL_ADDRESS	((vaddr_t) 0xFFFFF000)

/*
 * User stack starts at 64 MB — keeps text, heap, and stack within
 * the flat single-level page table region (16K PTEs, 64 KB).
 * Programs needing more VA (large mmap, shared libs) trigger
 * promotion to a 2-level page table.
 */
#define USRSTACK		((vaddr_t) 0x04000000)

/*
 * Flat page table threshold: VA range covered by single-level PT.
 * 64 MB = 16384 pages × 4 bytes/PTE = 64 KB page table.
 */
#define PENUMBRA_PT1_LIMIT	((vaddr_t) 0x04000000)

/* Kernel text load address: 64K above kernel base (null guard) */
#define KERNEL_TEXT_BASE	((vaddr_t) 0x80010000)

/*
 * UVM constants.
 */
#define VM_PHYSSEG_MAX		4	/* RAM + ROM + device regions */
#define VM_NFREELIST		1
#define VM_FREELIST_DEFAULT	0

/*
 * Process size limits.
 */
#define MAXTSIZ		(64 * 1024 * 1024)	/* max text size (64 MB) */
#define DFLDSIZ		(128 * 1024 * 1024)	/* default data size (128 MB) */
#define MAXDSIZ		(512 * 1024 * 1024)	/* max data size (512 MB) */
#define DFLSSIZ		(2 * 1024 * 1024)	/* default stack size (2 MB) */
#define MAXSSIZ		(32 * 1024 * 1024)	/* max stack size (32 MB) */

#endif /* _PENUMBRA_VMPARAM_H_ */
