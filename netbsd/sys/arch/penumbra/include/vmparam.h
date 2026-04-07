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
 * Kernel VA:  0x8000_0000 – 0xFFFC_FFFF  (2 GB - 12K)
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
 * Top three pages are reserved for the TLB miss handler:
 *   0xFFFF_F000  unmapped guard (catches (void *)-1 derefs)
 *   0xFFFF_E000  L1 pinned TLB slot (current page table L1)
 *   0xFFFF_D000  L2 window pinned TLB slot (transient L2 mapping)
 *
 * Page tables are always 2-level: L1 (1024 entries, 4 KB)
 * → L2 tables (1024 entries each, 4 KB, covering 4 MB per table).
 * The TLB miss handler is stateless — it always walks 2-level,
 * regardless of address space or VA range.
 *
 * Kernel VA includes device MMIO near the top
 * (ROM at 0xFFFF_0000, UART at 0xFF00_0000 physical).
 */

/* Page size: 4 KB (matches Penumbra MMU hardware) */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1 << PAGE_SHIFT)
#define PAGE_MASK	(PAGE_SIZE - 1)

/* User virtual address range */
#define VM_MIN_ADDRESS		((vaddr_t) 0x00001000)
#define VM_MAX_ADDRESS		((vaddr_t) 0xFFFFFFFF)
#define VM_MAXUSER_ADDRESS	((vaddr_t) 0x80000000)

/* Kernel virtual address range (top 3 pages reserved for TLB handler) */
#define VM_MIN_KERNEL_ADDRESS	((vaddr_t) 0x80000000)
#define VM_MAX_KERNEL_ADDRESS	((vaddr_t) 0xFFFFD000)

/*
 * User stack starts at 64 MB — keeps text, heap, and stack
 * within a compact VA range for TLB efficiency.
 */
#define USRSTACK		((vaddr_t) 0x04000000)

/* Kernel text load address: 64K above kernel base (null guard) */
#define KERNEL_TEXT_BASE	((vaddr_t) 0x80010000)

/*
 * UVM constants.
 */
#define VM_PHYSSEG_MAX		4	/* RAM + ROM + device regions */
#define VM_PHYSSEG_STRAT	VM_PSTRAT_BSEARCH
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
