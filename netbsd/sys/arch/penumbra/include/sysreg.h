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
#define MMU_TLB_INDEX		5	/* Selects TLB slot; bit 6 = pinned */

/* MMUCR fields */
#define MMUCR_M			0x0001	/* MMU enable */
#define MMUCR_ASID_SHIFT	8
#define MMUCR_ASID_MASK		0xFF00

/* FAULT_STATUS bit positions */
#define FSTAT_R		8	/* Faulting access was read */
#define FSTAT_W		9	/* Faulting access was write */
#define FSTAT_X		10	/* Faulting access was execute */
#define FSTAT_USR	11	/* Faulting access was user mode */

/* ── TLB geometry ───────────────────────────────────────────────────── */

#define TLB_NSLOTS	64	/* Total main TLB slots */
#define TLB_NSETS	32	/* Sets (indexed by VPN[4:0]) */
#define TLB_NWAYS	2	/* Ways per set */
#define PTLB_NSLOTS	8	/* Pinned TLB slots (fully associative) */
#define TLB_INDEX_PINNED 0x40	/* Set bit 6 in TLB_INDEX to target pinned TLB */

/*
 * Pinned TLB slot assignments (TLB_INDEX = TLB_INDEX_PINNED | slot).
 *
 * The TLB miss walker (in the pinned vector page) branches on the
 * faulting VA's MSB and uses either KERN_L1 or USER_L1 — SH-4-style
 * split walker.  KERN_L1 is pinned once at boot and never moves;
 * USER_L1 is reprogrammed by pmap_activate on every context switch.
 */
#define PTLB_VECTOR	(TLB_INDEX_PINNED | 0)	/* vector page (VECTOR_VA) */
#define PTLB_KERN_L1	(TLB_INDEX_PINNED | 1)	/* kernel L1 (permanent) */
#define PTLB_L2WIN	(TLB_INDEX_PINNED | 2)	/* L2 window (TLB handler) */
#define PTLB_SCRATCH	(TLB_INDEX_PINNED | 3)	/* scratch window (C code) */
#define PTLB_USER_L1	(TLB_INDEX_PINNED | 4)	/* current user L1 (or V=0) */
/* Slots 5..7 are currently unused. */

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
