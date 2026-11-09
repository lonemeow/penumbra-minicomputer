/*	$NetBSD$	*/

/*
 * Penumbra system register definitions.
 *
 * Device and register numbers for RDSYS/WRSYS instructions.
 * Safe for inclusion from both C and assembly (.S) files.
 *
 * See doc/system/sysregs.md for the full specification.
 */

#ifndef _PENUMBRA_SYSREG_H_
#define _PENUMBRA_SYSREG_H_

/* ── Device numbers (RDSYS/WRSYS dev field) ─────────────────────────── */

#define SYSDEV_MMU		0	/* MMU / TLB management */
#define SYSDEV_CPU		1	/* CPU identity + perfctrs (read-only id) */
#define SYSDEV_L1_DCACHE	2	/* L1 D-cache control */
#define SYSDEV_L1_ICACHE	3	/* L1 I-cache control */
#define SYSDEV_BUS		4	/* Bus controller (autoconfig) */
/* Devices 5-6 reserved */
#define SYSDEV_TIMER		7	/* Programmable interval timer */
#define SYSDEV_MACH		8	/* Machine identity (board name, CPU clock freq) */
#define SYSDEV_L2_CACHE		9	/* L2 unified cache control */
/* Devices 10-14 reserved */
#define SYSDEV_DEBUG		15	/* ISS-only debug (watchpoint); no-op on hardware */

/* ── Device 15: ISS debug registers (simulator only) ───────────────── */

#define DBG_WATCH_PA	0	/* Add PA to watchpoint list (0=clear all) */
#define DBG_WATCH_VAL	1	/* Value filter: only trigger on this value (0xFFFFFFFF=any) */

/* ── Device 0: MMU registers ────────────────────────────────────────── */

#define MMU_MMUCR		0	/* Control: bit 0 = M (enable), [15:8] = ASID */
#define MMU_FAULT_ADDR		1	/* Faulting VA (read-only, latched on fault) */
#define MMU_FAULT_STATUS	2	/* Fault type + access info (read-only) */
#define MMU_TLB_VPN		3	/* Staged TLB upper word (VPN + ASID) */
#define MMU_TLB_PTE		4	/* TLB lower word; write commits entry */
#define MMU_TLB_INDEX		5	/* Selects TLB slot, pinned or main */
#define MMU_TLB_INVAL_VA	6	/* v2: invalidate what a TLB_VPN word hits */
#define MMU_TLB_INVAL_ASID	7	/* v2: invalidate an ASID's non-global entries */
#define MMU_TLB_INVAL_ALL	8	/* v2: invalidate the main TLB */
#define MMU_CONFIG		15	/* Interface version and translation geometry */

/* MMU_CONFIG fields */
#define MMUCFG_VER_MASK		0x0000000F
#define MMUCFG_PINNED_SHIFT	4
#define MMUCFG_SETS_LOG2_SHIFT	10
#define MMUCFG_WAYS_SHIFT	15
#define MMUCFG_PAGE_LOG2_SHIFT	20
#define MMUCFG_ASID_BITS_SHIFT	25

#define MMUCFG_VER(c)		((c) & MMUCFG_VER_MASK)
#define MMUCFG_PINNED(c)	(((c) >> MMUCFG_PINNED_SHIFT) & 0x3F)
#define MMUCFG_SETS_LOG2(c)	(((c) >> MMUCFG_SETS_LOG2_SHIFT) & 0x1F)
#define MMUCFG_WAYS(c)		(((c) >> MMUCFG_WAYS_SHIFT) & 0x1F)
#define MMUCFG_PAGE_LOG2(c)	(((c) >> MMUCFG_PAGE_LOG2_SHIFT) & 0x1F)
#define MMUCFG_ASID_BITS(c)	(((c) >> MMUCFG_ASID_BITS_SHIFT) & 0x1F)

/*
 * The MMUs the kernel drives, each the MMU_CONFIG value that reports
 * it.  The miss handler carries the geometry as instruction immediates,
 * so locore.S builds tlb_refill.S once for each; a machine reporting
 * anything else stops at boot.  A read of zero reports
 * MMUCFG_V1_8P_32X2.
 */
#define MMUCFG_V1_8P_32X2	0x10C11481 /* v1: 8 pinned, 32x2, 4 KB, 8-bit ASID */
#define MMUCFG_V2_8P_256X2	0x10C12082 /* v2: 8 pinned, 256x2, 4 KB, 8-bit ASID */

/* MMUCR fields */
#define MMUCR_M			0x0001	/* MMU enable */
#define MMUCR_ASID_SHIFT	8
#define MMUCR_ASID_MASK		0xFF00

/* FAULT_STATUS[3:0]: fault type */
#define FSTAT_TYPE_MASK		0x000F
#define FSTAT_TYPE_TLB_MISS	0x1
#define FSTAT_TYPE_PROT		0x2
#define FSTAT_TYPE_ALIGN	0x3
#define FSTAT_TYPE_BUS		0x4
#define FSTAT_TYPE_BUS_DEFERRED	0x5	/* FAULT_ADDR is physical */

/* FAULT_STATUS bit positions */
#define FSTAT_R		8	/* Faulting access was read */
#define FSTAT_W		9	/* Faulting access was write */
#define FSTAT_X		10	/* Faulting access was execute */
#define FSTAT_USR	11	/* Faulting access was user mode */

/* ── TLB geometry ───────────────────────────────────────────────────── */

/*
 * TLB_INDEX selects main slot way * sets + set as that number, and
 * pinned slot n as the version's pinned select | n (doc/system/mmu.md,
 * "TLB_INDEX").  The kernel keeps its MMU's select in
 * tlb_pinned_select.
 */
#define TLB_INDEX_PINNED_V1	0x40
#define TLB_INDEX_PINNED_V2	0x80000000

#define TLB_V1_NSLOTS	64	/* version 1's 32 sets of 2 ways, slot by slot */
#define PTLB_NSLOTS	8	/* Pinned TLB slots (fully associative) */

/*
 * Pinned TLB slot assignments, as slot numbers; the TLB_INDEX of slot
 * n is tlb_pinned_select | n.
 *
 * The TLB miss walker (in the pinned vector page) branches on the
 * faulting VA's MSB and uses either KERN_L1 or USER_L1 — SH-4-style
 * split walker.  KERN_L1 is pinned once at boot and never moves;
 * USER_L1 is reprogrammed by pmap_activate on every context switch.
 */
#define PTLB_SLOT_VECTOR	0	/* vector page (VECTOR_VA) */
#define PTLB_SLOT_KERN_L1	1	/* kernel L1 (permanent) */
#define PTLB_SLOT_L2WIN		2	/* L2 window (TLB handler) */
#define PTLB_SLOT_SCRATCH	3	/* scratch window (C code) */
#define PTLB_SLOT_USER_L1	4	/* current user L1 (or V=0) */
#define PTLB_SLOT_SCRATCH2	5	/* second scratch window */

/*
 * Slots below this are structural: translation, or an operation the
 * kernel cannot express without them, depends on each.  Anything the
 * MMU provides beyond them is free to spend on pages the kernel
 * touches constantly.
 */
#define PTLB_NSTRUCTURAL 6

/* The page holding cpu_info_store, which every trap reads and writes. */
#define PTLB_SLOT_CPU_INFO	6

/* TLB_VPN word: (VPN << 8) | ASID */
#define TLB_VPN_SHIFT	8

/* TLB_PTE word: (PPN << 12) | (SW << 8) | flags */
/* Flag and PPN bits defined in <machine/pmap.h> */

/* ── Device 1: CPU identity + performance counters ─────────────────── */

#define CPU_ISA			0
#define CPU_NAME0		1
#define CPU_NAME1		2
#define CPU_NAME2		3
#define CPU_NAME3		4
#define CPU_CYCLES		5	/* free-running 32-bit, wraps every ~5.7 min @ 12.5 MHz */
#define CPU_INSNS_RETIRED	6	/* free-running 32-bit */
#define CPU_STALL_FUNIT		7	/* free-running 32-bit stall counters */
#define CPU_STALL_IFETCH	8
#define CPU_STALL_LOAD		9
#define CPU_STALL_STORE		10
#define CPU_STALL_HAZARD	11
#define CPU_STALL_FLUSH		12

/* Bulk-read contract: the machdep.cpu.all sysctl returns every CPU
 * perfctr in a single call as uint64_t[CPU_NPERFCTR] (free-running
 * 32-bit values zero-extended), indexed by CPU_PERF_*.  Lets a poller
 * read the whole set in one syscall instead of one per counter; the
 * individual machdep.cpu.<name> leaves remain for ad-hoc sysctl(8) use. */
#define CPU_PERF_CYCLES		0
#define CPU_PERF_INSNS		1
#define CPU_PERF_STALL_FUNIT	2
#define CPU_PERF_STALL_IFETCH	3
#define CPU_PERF_STALL_LOAD	4
#define CPU_PERF_STALL_STORE	5
#define CPU_PERF_STALL_HAZARD	6
#define CPU_PERF_STALL_FLUSH	7
#define CPU_NPERFCTR		8

/* CPU_ISA layout: bits [3:0] = ISA version, bits [31:4] = feature flags
 * (bit indices below are relative to bit 4 of the register). */
#define CPU_FEAT_BIT_FPU	0	/* Floating-point unit */

/* ── Device 8: Machine identity (read-only) ────────────────────────── */

#define MACH_FEAT	0
#define MACH_NAME0	1
#define MACH_NAME1	2
#define MACH_NAME2	3
#define MACH_NAME3	4
#define MACH_CPU_FREQ	5

/* ── Cache devices (L1_DCACHE=2, L1_ICACHE=3, L2_CACHE=9) — shared layout ─ */

#define CACHE_INFO		0	/* R  — geometry; 0 = absent */
#define CACHE_CTRL		1	/* RW — bit 0 = ENABLE */
#define CACHE_INVAL_ALL		2	/* W  — any value drops all lines */
#define CACHE_INVAL_LINE	3	/* W  — physical addr; drop matching line */
#define CACHE_FLUSH_ALL		4	/* W  — writeback dirty (WB caches) */
#define CACHE_FLUSH_LINE	5	/* W  — writeback one line (WB caches) */
#define CACHE_STATUS		6	/* R  — bit 0 = multi-cycle op busy */
/* Regs 7-9 reserved for future control (perfctr CTRL, WB-buffer status, ...) */
#define CACHE_READ_HITS		10	/* R  — read accesses that hit a valid line */
#define CACHE_READ_MISSES	11	/* R  — read accesses that missed */
#define CACHE_WRITE_HITS	12	/* R  — write accesses that hit a valid line */
#define CACHE_WRITE_MISSES	13	/* R  — write accesses that missed */
/* Regs 14-15 reserved for future counters (LINE_FILLS, WRITEBACKS, ...) */

/* Bulk-read contract: each cache's machdep.cache.<dev>.all sysctl
 * returns its four counters in one call as uint64_t[CACHE_NPERFCTR],
 * indexed by CACHE_PERF_*.  The per-counter machdep.cache.<dev>.<name>
 * leaves remain for ad-hoc sysctl(8) use. */
#define CACHE_PERF_READ_HITS	0
#define CACHE_PERF_READ_MISSES	1
#define CACHE_PERF_WRITE_HITS	2
#define CACHE_PERF_WRITE_MISSES	3
#define CACHE_NPERFCTR		4

#define CACHE_CTRL_ENABLE	0x01
#define CACHE_STATUS_BUSY	0x01

/* CACHE_INFO field layout (unified across all cache devices).
 * See hw/rtl/core/penumbra_pkg.sv for the canonical encoding. */
#define CACHE_INFO_LINE_WORDS(v)	(((v) >>  0) & 0x003Fu)
#define CACHE_INFO_NUM_SETS(v)		(((v) >>  6) & 0x7FFFu)
#define CACHE_INFO_NUM_WAYS(v)		(((v) >> 21) & 0x001Fu)
#define CACHE_INFO_ADDRESSING(v)	(((v) >> 26) & 0x0003u)
#define CACHE_INFO_WRITE_BACK(v)	(((v) >> 28) & 0x0001u)
#define CACHE_INFO_WRITE_ALLOC(v)	(((v) >> 29) & 0x0001u)

#define CACHE_ADDR_PIPT		0
#define CACHE_ADDR_VIPT		1
#define CACHE_ADDR_VIVT		2

/* ── Device 4: Bus controller ───────────────────────────────────────── */

#define BUS_CTL		0	/* bit 0 = RST, bit 1 = CFG_EN */

#define BUSCTL_RST	0x01
#define BUSCTL_CFG_EN	0x02

/* ── Device 7: Timer registers ─────────────────────────────────────── */

#define TM_FREQ		0	/* Tick frequency in Hz (read-only) */
#define TM_CR		1	/* Control register */
#define TM_COUNT	2	/* Current 16-bit counter (counts down) */
#define TM_RELOAD	3	/* 16-bit reload value */
#define TM_STATUS	4	/* Status: bit 0 = UDF (write-1-to-clear) */

/* TMCR bit masks */
#define TMCR_TICK_EN	0x01	/* Enable counting */
#define TMCR_IRQ_EN	0x02	/* Enable interrupt output */
#define TMCR_AUTOLOAD	0x04	/* Auto-reload on underflow */

/* TMSTATUS bit masks */
#define TMST_UDF	0x01	/* Underflow flag (write-1-to-clear) */

#endif /* _PENUMBRA_SYSREG_H_ */
