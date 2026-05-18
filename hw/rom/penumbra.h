/*
 * penumbra.h — Inline asm helpers for Penumbra system registers
 *
 * Provides C-callable access to SPRs (via RDSPR/WRSPR) and device
 * sysregs (via RDSYS/WRSYS) that would otherwise require handwritten
 * assembly.
 *
 * SPR/sysreg numbers are encoded as instruction immediates, so they
 * must be compile-time constants.  We use macros with token-paste
 * stringification to embed the number directly in the asm template,
 * bypassing the "i" constraint (which doesn't survive -O0/optnone).
 * This is the same technique Linux uses for ARM64 read_sysreg().
 */

#ifndef PENUMBRA_H
#define PENUMBRA_H

/* Freestanding — no stdint.h.  Penumbra is ILP32: int = long = 32 bits. */
typedef unsigned int uint32_t;

/* ── Stringification helper ──────────────────────────────────────────── */
#define _PENUMBRA_STR(x) #x
#define PENUMBRA_STR(x)  _PENUMBRA_STR(x)

/* ── Special-Purpose Registers (RDSPR/WRSPR) ────────────────────────── */

#define SPR_ESR  0   /* Exception Status Register (saved SR) */
#define SPR_EPC  1   /* Exception PC */
#define SPR_USP  2   /* User Stack Pointer (banked R14) */

#define penumbra_read_spr(spr) ({                                       \
    uint32_t __val;                                                     \
    asm volatile("rdspr %0, " PENUMBRA_STR(spr) : "=r"(__val));         \
    __val;                                                              \
})

#define penumbra_write_spr(spr, val) do {                               \
    uint32_t __v = (val);                                               \
    asm volatile("wrspr " PENUMBRA_STR(spr) ", %0" : : "r"(__v));       \
} while (0)

/* ── Device System Registers (RDSYS/WRSYS) ──────────────────────────
 *
 * Device 0 = MMU:
 *   0 = MMUCR        (control: enable bit, ASID)
 *   1 = FAULT_ADDR   (faulting virtual address, read-only)
 *   2 = FAULT_STATUS (fault type + access info bits, read-only)
 *
 * Device 1 = System ID (read-only)
 */

#define SYSDEV_MMU    0
#define SYSDEV_CPU    1
#define SYSDEV_DCACHE 2
#define SYSDEV_ICACHE 3
#define SYSDEV_MACH   8

#define MMU_CR           0
#define MMU_FAULT_ADDR   1
#define MMU_FAULT_STATUS 2

/* CPU identity registers (device 1, read-only) */
#define CPU_ISA           0
#define CPU_NAME0         1
#define CPU_NAME1         2
#define CPU_NAME2         3
#define CPU_NAME3         4
/* CPU performance counters (free-running, 32-bit) */
#define CPU_CYCLES        5
#define CPU_INSNS_RETIRED 6

/* Machine identity registers (device 8, read-only) */
#define MACH_FEAT      0
#define MACH_NAME0     1
#define MACH_NAME1     2
#define MACH_NAME2     3
#define MACH_NAME3     4
#define MACH_CPU_FREQ  5

/* CPU_ISA feature flag bit indices */
#define CPU_FEAT_BIT_HW_MUL  0
#define CPU_FEAT_BIT_HW_DIV  1
#define CPU_FEAT_BIT_FPU     2

/* ── Cache devices — shared layout across L1 D/I, L2, future L3 ──── */
#define SYSDEV_L2     9

/* Register map (same for every cache device).  Reading INFO=0 means
 * the device is absent (either not instantiated or device id unmapped),
 * so software probes presence with a single RDSYS. */
#define CACHE_INFO         0   /* R  — geometry; 0 = absent */
#define CACHE_CTRL         1   /* RW — bit 0 = enable */
#define CACHE_INVAL_ALL    2   /* W  — any value drops all lines */
#define CACHE_INVAL_LINE   3   /* W  — physical addr; drop matching line */
#define CACHE_FLUSH_ALL    4   /* W  — writeback dirty (WB caches only) */
#define CACHE_FLUSH_LINE   5   /* W  — writeback one line  (WB caches only) */
#define CACHE_STATUS       6   /* R  — bit 0 = busy (multi-cycle op in progress) */

#define CACHE_CTRL_ENABLE   0x01
#define CACHE_STATUS_BUSY   0x01

/* CACHE_INFO field layout (matches penumbra_pkg.sv unified encoding).
 * 32-bit register, fields packed LSB-first so adding caches with
 * different geometry doesn't move the rest. */
#define CACHE_INFO_LINE_WORDS(v)   (((v) >>  0) & 0x003Fu)  /* 1..63   */
#define CACHE_INFO_NUM_SETS(v)     (((v) >>  6) & 0x7FFFu)  /* 1..32767 */
#define CACHE_INFO_NUM_WAYS(v)     (((v) >> 21) & 0x001Fu)  /* 1..31   */
#define CACHE_INFO_ADDRESSING(v)   (((v) >> 26) & 0x0003u)  /* PIPT/VIPT/VIVT */
#define CACHE_INFO_WRITE_BACK(v)   (((v) >> 28) & 0x0001u)  /* 0=WT, 1=WB */
#define CACHE_INFO_WRITE_ALLOC(v)  (((v) >> 29) & 0x0001u)  /* 0=WnA, 1=WA */

#define CACHE_ADDR_PIPT 0
#define CACHE_ADDR_VIPT 1
#define CACHE_ADDR_VIVT 2

/* FAULT_STATUS bit positions */
#define FSTAT_R    8   /* Faulting access was read */
#define FSTAT_W    9   /* Faulting access was write */
#define FSTAT_X   10   /* Faulting access was execute (fetch) */
#define FSTAT_USR 11   /* Faulting access was user mode */

#define penumbra_read_sysreg(dev, reg) ({                               \
    uint32_t __val;                                                     \
    asm volatile("rdsys %0, " PENUMBRA_STR(dev) ", " PENUMBRA_STR(reg)  \
                 : "=r"(__val));                                        \
    __val;                                                              \
})

#define penumbra_write_sysreg(dev, reg, val) do {                       \
    uint32_t __v = (val);                                               \
    asm volatile("wrsys %0, " PENUMBRA_STR(dev) ", " PENUMBRA_STR(reg)  \
                 : : "r"(__v));                                         \
} while (0)

/* ── Bus Controller (SYSDEV_BUS, device 4) ──────────────────────────── */

#define SYSDEV_BUS    4
#define BUS_CTL       0   /* BUSCTL: bit 0 = RST, bit 1 = CFG_EN */

#define BUSCTL_RST    0x1
#define BUSCTL_CFG_EN 0x2

/* Bus reset pulse minimum: 100 µs (see doc/hardware/bus-protocol.md).
 * Each volatile loop iteration is ~25-30 cycles (load, compare, branch,
 * increment, store).  At 25 MHz that's ~1 µs/iteration.
 * 200 iterations ≈ 200 µs — comfortably above the 100 µs spec.
 * Adjust if system clock changes significantly.                         */
#define BUS_RESET_DELAY_ITERS  200

/* ── Autoconfig config space (0xFE00_0000, active when CFG_EN) ──────── */

#define AUTOCONFIG_BASE   0xFE000000
#define ACFG_CLASS  (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x00))
#define ACFG_SIZE   (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x04))
#define ACFG_ID     (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x08))
#define ACFG_NAME0  (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x0C))
#define ACFG_NAME1  (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x10))
#define ACFG_NAME2  (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x14))
#define ACFG_NAME3  (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x18))
#define ACFG_BASE   (*(volatile uint32_t *)(AUTOCONFIG_BASE + 0x1C))

/* Device class codes */
#define ACFG_CLASS_UNKNOWN  0
#define ACFG_CLASS_MEMORY   1
#define ACFG_CLASS_UART     2
#define ACFG_CLASS_SPI      3
#define ACFG_CLASS_SD       4

/* ── Built-in device addresses (hardwired, not autoconfigured) ──────── */
#define UART_ADDR  0xFF000000

/* ── Interrupt control ───────────────────────────────────────────────── */

static inline void disable_interrupts(void) {
    asm volatile("di" ::: "memory");
}

static inline void enable_interrupts(void) {
    asm volatile("ei" ::: "memory");
}

#endif /* PENUMBRA_H */
