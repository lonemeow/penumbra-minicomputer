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

#define SYSDEV_MMU	0	/* MMU / TLB management */
#define SYSDEV_CPU	1	/* CPU identity + perfctrs (read-only id) */
#define SYSDEV_DCACHE	2	/* D-cache control */
#define SYSDEV_ICACHE	3	/* I-cache control */
#define SYSDEV_BUS	4	/* Bus controller (autoconfig) */
/* Devices 5-6 reserved for future cache levels (L2, L3) */
#define SYSDEV_TIMER	7	/* Programmable interval timer */
#define SYSDEV_MACH	8	/* Machine identity (board name, CPU clock freq) */
/* Devices 9-14 reserved */
#define SYSDEV_DEBUG	15	/* ISS-only debug (watchpoint); no-op on hardware */

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
#define PTLB_NSLOTS	4	/* Pinned TLB slots (fully associative) */
#define TLB_INDEX_PINNED 0x40	/* Set bit 6 in TLB_INDEX to target pinned TLB */

/* Pinned TLB slot assignments (TLB_INDEX = TLB_INDEX_PINNED | slot) */
#define PTLB_VECTOR	(TLB_INDEX_PINNED | 0)	/* vector page (VECTOR_VA) */
#define PTLB_L1		(TLB_INDEX_PINNED | 1)	/* kernel L1 page table */
#define PTLB_L2WIN	(TLB_INDEX_PINNED | 2)	/* L2 window (TLB handler) */
#define PTLB_SCRATCH	(TLB_INDEX_PINNED | 3)	/* scratch window (C code) */

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

/* CPU_ISA layout: bits [3:0] = ISA version, bits [31:4] = feature flags */
#define CPU_FEAT_BIT_HW_MUL	0	/* Hardware multiply */
#define CPU_FEAT_BIT_HW_DIV	1	/* Hardware divide */
#define CPU_FEAT_BIT_FPU	2	/* Floating-point unit */

/* ── Device 8: Machine identity (read-only) ────────────────────────── */

#define MACH_FEAT	0
#define MACH_NAME0	1
#define MACH_NAME1	2
#define MACH_NAME2	3
#define MACH_NAME3	4
#define MACH_CPU_FREQ	5

/* ── Device 2/3: Cache registers ────────────────────────────────────── */

#define CACHE_GEOM	0	/* Geometry (read-only) — alias for CACHE_INFO */
#define CACHE_INFO	0	/* Geometry/type (read-only) */
#define CACHE_CTRL	1	/* bit 0 = ENABLE */
#define CACHE_INVAL	2	/* Write to invalidate all */

#define CACHE_CTRL_ENABLE	0x01

/* CACHE_INFO field layout (matches hw/rtl/soc/cache.sv INFO_VALUE) */
#define CACHE_INFO_LINE_WORDS(v)	(((v) >>  0) & 0x000Fu)
#define CACHE_INFO_NUM_SETS(v)		(((v) >>  4) & 0x03FFu)
#define CACHE_INFO_NUM_WAYS(v)		(((v) >> 14) & 0x000Fu)
#define CACHE_INFO_TYPE(v)		(((v) >> 18) & 0x0003u)	/* PIPT/VIPT/VIVT */
#define CACHE_INFO_WRITE_BACK(v)	(((v) >> 20) & 0x0001u)	/* 0=WT, 1=WB */
#define CACHE_INFO_WRITE_ALLOC(v)	(((v) >> 21) & 0x0001u)	/* 0=WnA, 1=WA */

#define CACHE_TYPE_PIPT		0
#define CACHE_TYPE_VIPT		1
#define CACHE_TYPE_VIVT		2

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
