/*	$NetBSD$	*/

/*
 * Penumbra system register definitions.
 *
 * Device and register numbers for RDSYS/WRSYS instructions.
 * Safe for inclusion from both C and assembly (.S) files.
 *
 * See doc/isa/sysregs-reference.md for the full specification.
 */

#ifndef _PENUMBRA_SYSREG_H_
#define _PENUMBRA_SYSREG_H_

/* ── Device numbers (RDSYS/WRSYS dev field) ─────────────────────────── */

#define SYSDEV_MMU	0	/* MMU / TLB management */
#define SYSDEV_SYSID	1	/* CPU and machine ID (read-only) */
#define SYSDEV_DCACHE	2	/* D-cache control */
#define SYSDEV_ICACHE	3	/* I-cache control */
#define SYSDEV_BUS	4	/* Bus controller (autoconfig) */
/* Devices 5-6 reserved for future cache levels (L2, L3) */
#define SYSDEV_TIMER	7	/* Programmable interval timer */

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

/* ── Device 1: System ID registers (read-only) ─────────────────────── */

#define SYS_CPU_ISA	0
#define SYS_MACH_FEAT	1
#define SYS_CPU_NAME0	2
#define SYS_CPU_NAME1	3
#define SYS_CPU_NAME2	4
#define SYS_CPU_NAME3	5
#define SYS_MACH_NAME0	6
#define SYS_MACH_NAME1	7
#define SYS_MACH_NAME2	8
#define SYS_MACH_NAME3	9

/* ── Device 2/3: Cache registers ────────────────────────────────────── */

#define CACHE_CTRL	0	/* bit 0 = ENABLE */
#define CACHE_GEOM	1	/* Geometry (read-only) */
#define CACHE_INVAL	2	/* Write to invalidate all */

#define CACHE_CTRL_ENABLE	0x01

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
