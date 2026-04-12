/*
 * crt0.S — Bare-metal benchmark startup for Penumbra
 *
 * Entry state (from ROM boot command):
 *   R1 = physical pointer to boot data
 *   Supervisor mode, MMU off, interrupts off
 *
 * Startup phases:
 *   1. PIE self-relocation (process R_PENUMBRA_RELATIVE)
 *   2. Copy TLB miss + IRQ handlers to page 0x1000
 *   3. Install vector table entries, pin handler page + page 0
 *   4. Identity-map current code page, enable MMU (for caching)
 *   5. Set up stack, call bench_main(bootdata)
 */

/* ── ELF dynamic section tags ─────────────────────────────────── */
#define DT_NULL             0
#define DT_REL              17
#define DT_RELSZ            18
#define R_PENUMBRA_RELATIVE 8

/* ── MMU sysregs (device 0) ───────────────────────────────────── */
#define SYSDEV_MMU          0
#define MMU_MMUCR           0
#define MMU_FAULT_ADDR      1
#define MMU_TLB_VPN         3
#define MMU_TLB_PTE         4
#define MMU_TLB_INDEX       5
#define MMUCR_M             1

/* ── Timer sysregs (device 7) ─────────────────────────────────── */
#define SYSDEV_TIMER        7
#define TM_STATUS           4

/* ── PTE flags (must match hardware TLB_PTE layout) ───────────── */
#define PTE_KERNEL          0xBD    /* V|C|R|W|X|G */
#define PTE_KERNEL_NC       0x99    /* V|R|W|G (uncached, for MMIO) */

/* ── Pinned TLB ───────────────────────────────────────────────── */
#define TLB_PINNED          0x40

/* ── Fixed addresses ──────────────────────────────────────────── */
#define HANDLER_PAGE        0x1000
#define TIMER_BASE_ADDR    0x38    /* accumulated ticks, page 0 */
#define STACK_TOP           0x00100000  /* 1 MB */


/* ═════════════════════════════════════════════════════════════════
 * Phase 1: Entry + PIE self-relocation
 *
 * Same algorithm as the bootloader (stand/boot/crt0.S):
 * compute load bias from _DYNAMIC, walk .rel.dyn, patch
 * R_PENUMBRA_RELATIVE entries.
 * ═════════════════════════════════════════════════════════════════ */

        .section .text._start,"ax",@progbits
        .globl  _start
        .type   _start,@function

_start:
        mov     r5, r1                  /* save bootdata pointer */
        li      sp, 0x00080000          /* temp stack at 512 KB */

        /* Compute runtime address of _DYNAMIC (PC-relative) */
        mov     r2, pc
        add     r2, #8
        add     r2, #%pcrel(_DYNAMIC)

        /* Load link-time _DYNAMIC from .data (pre-relocation) */
        mov     r3, pc
        add     r3, #8
        add     r3, #%pcrel(.Ldynamic_linkaddr)
        ldw     r3, [r3]

        /* Load bias = runtime - linktime */
        mov     r4, r2
        sub     r4, r3

        cmp     r4, #0
        beq     .Lreloc_done

        /* Walk .dynamic for DT_REL and DT_RELSZ */
        lli     r6, #0
        lli     r7, #0

.Ldyn_loop:
        ldw     r8, [r2]
        cmp     r8, #DT_NULL
        beq     .Ldyn_done
        ldw     r9, [r2 + 4]
        cmp     r8, #DT_REL
        beq     .Lfound_rel
        cmp     r8, #DT_RELSZ
        beq     .Lfound_relsz
        add     r2, #8
        b       .Ldyn_loop

.Lfound_rel:
        mov     r6, r9
        add     r2, #8
        b       .Ldyn_loop

.Lfound_relsz:
        mov     r7, r9
        add     r2, #8
        b       .Ldyn_loop

.Ldyn_done:
        add     r6, r4                  /* runtime &.rel.dyn */
        add     r7, r6                  /* end of .rel.dyn */

.Lrel_loop:
        cmp     r6, r7
        bhs     .Lreloc_done
        ldw     r8, [r6]               /* r_offset */
        ldw     r9, [r6 + 4]            /* r_info */
        and     r9, #0xFF              /* type */
        cmp     r9, #R_PENUMBRA_RELATIVE
        bne     .Lrel_next
        add     r8, r4                  /* runtime address */
        ldw     r9, [r8]
        add     r9, r4                  /* add bias */
        stw     r9, [r8]
.Lrel_next:
        add     r6, #8
        b       .Lrel_loop

.Lreloc_done:

/* ═════════════════════════════════════════════════════════════════
 * Phase 2: Copy handlers to page 0x1000
 *
 * The handler block (_handler_start to _handler_end) is a
 * self-contained unit using PC-relative scratch addressing.
 * Copying preserves relative offsets — works at any address.
 *
 * Symbol addresses are loaded from .data slots (patched by the
 * self-relocator above) via PC-relative indirection — same
 * technique the bootloader uses for _DYNAMIC.
 * ═════════════════════════════════════════════════════════════════ */

        /* Clear overflow counter */
        stw     r0, [r0 + TIMER_BASE_ADDR]

        /* Load runtime addresses of handler block bounds.
         * .Laddr_* slots in .data were patched by the relocator. */

        /* r1 = &_handler_start (runtime) */
        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(.Laddr_handler_start)
        ldw     r1, [r1]

        /* r2 = &_handler_end (runtime) */
        mov     r2, pc
        add     r2, #8
        add     r2, #%pcrel(.Laddr_handler_end)
        ldw     r2, [r2]

        /* Copy handler block word-by-word to HANDLER_PAGE */
        li      r3, HANDLER_PAGE

.Lcopy_loop:
        cmp     r1, r2
        bhs     .Lcopy_done
        ldw     r4, [r1]
        stw     r4, [r3]
        add     r1, #4
        add     r3, #4
        b       .Lcopy_loop
.Lcopy_done:

        /* Install vector table entries.
         * Compute: HANDLER_PAGE + (symbol - _handler_start)
         */

        /* Load _handler_start into r6 for offset computations */
        mov     r6, pc
        add     r6, #8
        add     r6, #%pcrel(.Laddr_handler_start)
        ldw     r6, [r6]

        /* vector[2] = TLB miss handler */
        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(.Laddr_bench_tlb_miss)
        ldw     r1, [r1]
        sub     r1, r6
        add     r1, #HANDLER_PAGE
        stw     r1, [r0 + 8]            /* vector[2] at phys 0x08 */

        /* vector[1] = IRQ handler */
        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(.Laddr_bench_irq)
        ldw     r1, [r1]
        sub     r1, r6
        add     r1, #HANDLER_PAGE
        stw     r1, [r0 + 4]            /* vector[1] at phys 0x04 */

/* ═════════════════════════════════════════════════════════════════
 * Phase 3: Pin TLB slots
 *
 * Slot 0: handler page (0x1000) — TLB miss + IRQ code
 * Slot 1: page 0 (0x0000) — vectors, bootdata, overflow counter
 *
 * Both identity-mapped (VA = PA) so handlers can run without
 * nested TLB misses.
 * ═════════════════════════════════════════════════════════════════ */

        /* Slot 0: handler page at PA 0x1000 */
        lli     r1, #TLB_PINNED
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX

        lli     r1, #0x100              /* TLB_VPN = (0x1 << 8) | ASID=0 */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_VPN

        lli     r1, #0x10BD            /* TLB_PTE = PA 0x1000 | PTE_KERNEL */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        /* Slot 1: page 0 at PA 0x0000 */
        lli     r1, #(TLB_PINNED | 1)
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX

        lli     r1, #0                  /* TLB_VPN = 0 */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_VPN

        lli     r1, #PTE_KERNEL         /* TLB_PTE = PA 0 | PTE_KERNEL */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═════════════════════════════════════════════════════════════════
 * Phase 4: Identity-map current code page + enable MMU
 *
 * After WRSYS sets M=1, instruction fetches go through the TLB.
 * This identity-map entry (VA = PA) lets the PC continue
 * executing at the same physical address.  It will be naturally
 * evicted once the benchmark runs at other addresses (the TLB
 * miss handler refills on demand).
 * ═════════════════════════════════════════════════════════════════ */

.Lmmu_ref:
        mov     r1, pc                  /* physical address of this instr */
        shr     r1, #12                 /* PPN */

        /* TLB_INDEX = PPN[4:0] (set), way 0 */
        mov     r2, r1
        and     r2, #0x1F
        wrsys   r2, #SYSDEV_MMU, #MMU_TLB_INDEX

        /* TLB_VPN = (PPN << 8) | ASID=0 */
        mov     r2, r1
        shl     r2, #8
        wrsys   r2, #SYSDEV_MMU, #MMU_TLB_VPN

        /* TLB_PTE = (PPN << 12) | PTE_KERNEL */
        shl     r1, #12
        lli     r2, #PTE_KERNEL
        or      r1, r2
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        /* Enable MMU — identity mapping means we continue here */
        lli     r1, #MMUCR_M
        wrsys   r1, #SYSDEV_MMU, #MMU_MMUCR

/* ═════════════════════════════════════════════════════════════════
 * Phase 5: Set stack + call bench_main
 * ═════════════════════════════════════════════════════════════════ */

        li      sp, STACK_TOP
        mov     r1, r5                  /* bootdata pointer */
        bl      bench_main
        break                           /* halt CPU */

        .size   _start, . - _start


/* ── PIE address slots (.data, patched by self-relocator) ──────── */
        .data
        .align  4
.Ldynamic_linkaddr:
        .long   _DYNAMIC
.Laddr_handler_start:
        .long   _handler_start
.Laddr_handler_end:
        .long   _handler_end
.Laddr_bench_tlb_miss:
        .long   _bench_tlb_miss
.Laddr_bench_irq:
        .long   _bench_irq


/* ═════════════════════════════════════════════════════════════════
 * Handler code — copied to page 0x1000 at startup.
 *
 * This is a self-contained block: all scratch access is
 * PC-relative, so the code works correctly after being copied
 * to any page-aligned address.
 *
 * Layout after copy:
 *   +0x000  TLB miss handler (identity map, VA = PA)
 *           IRQ handler (timer overflow counter)
 *           Scratch data (register save, way counter)
 * ═════════════════════════════════════════════════════════════════ */

        .section .handlers,"ax",@progbits
        .balign 4
        .globl  _handler_start
        .globl  _handler_end

_handler_start:

/* ── TLB miss handler ──────────────────────────────────────────
 *
 * Identity-maps the faulting address: VA = PA.
 * Cached (PTE_KERNEL) for addresses below 0xFF000000.
 * Uncached (PTE_KERNEL_NC) for addresses >= 0xFF000000 (MMIO).
 * Uses alternating way counter for TLB replacement.
 * ────────────────────────────────────────────────────────────── */
        .globl  _bench_tlb_miss
_bench_tlb_miss:
        stw     r1, [pc + _rmh_save_r1 - .]
        stw     r2, [pc + _rmh_save_r2 - .]
        stw     r3, [pc + _rmh_save_r3 - .]

        /* Read faulting virtual address */
        rdsys   r1, #SYSDEV_MMU, #MMU_FAULT_ADDR

        /* VPN = fault_addr >> 12 */
        mov     r2, r1
        shr     r2, #12

        /* TLB_INDEX = set (VPN[4:0]) | way_counter */
        mov     r3, r2
        and     r3, #0x1F
        ldw     r1, [pc + _rmh_way_ctr - .]
        or      r3, r1
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_INDEX

        /* TLB_VPN = (VPN << 8) | ASID=0 */
        mov     r3, r2
        shl     r3, #8
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_VPN

        /* TLB_PTE: choose cached vs uncached by address range.
         * VPN >> 12 gives fault_addr[31:24].  If >= 0xFF, uncached. */
        mov     r3, r2
        shl     r3, #12                /* PPN << 12 (= PA page base) */
        mov     r1, r2
        shr     r1, #12                /* fault_addr[31:24] */
        cmp     r1, #0xFF
        bhs     .Luncached
        add     r3, #PTE_KERNEL
        b       .Lcommit
.Luncached:
        add     r3, #PTE_KERNEL_NC
.Lcommit:
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_PTE

        /* Toggle way counter (0 ↔ 0x20 = bit 5) */
        lli     r1, #0x20
        ldw     r2, [pc + _rmh_way_ctr - .]
        xor     r2, r1
        stw     r2, [pc + _rmh_way_ctr - .]

        ldw     r1, [pc + _rmh_save_r1 - .]
        ldw     r2, [pc + _rmh_save_r2 - .]
        ldw     r3, [pc + _rmh_save_r3 - .]
        eret


/* ── IRQ handler: timer tick accumulation ──────────────────────
 *
 * On timer underflow, clears the UDF flag and adds one full
 * period (0xFFFF = 65535 ticks, matching TMRELOAD=0xFFFE) to
 * the base tick counter at TIMER_BASE_ADDR (0x38) on page 0.
 *
 * Accumulating ticks directly avoids an expensive software
 * multiply in the readback function (no hardware MUL).
 * Only R1 is saved (to PC-relative scratch on handler page).
 * SR/flags restored automatically by ERET from ESR.
 * ────────────────────────────────────────────────────────────── */
        .balign 4
        .globl  _bench_irq
_bench_irq:
        stw     r1, [pc + _irq_save_r1 - .]

        /* Clear timer UDF (write-1-to-clear) */
        lli     r1, #1
        wrsys   r1, #SYSDEV_TIMER, #TM_STATUS

        /* Accumulate one period of ticks (0xFFFF = reload + 1) */
        ldw     r1, [r0 + TIMER_BASE_ADDR]
        add     r1, #0xFFFF
        stw     r1, [r0 + TIMER_BASE_ADDR]

        ldw     r1, [pc + _irq_save_r1 - .]
        eret


/* ── Scratch data (contiguous with code for PC-relative access) ─ */
        .balign 4
_rmh_save_r1:   .long 0
_rmh_save_r2:   .long 0
_rmh_save_r3:   .long 0
_rmh_way_ctr:   .long 0
_irq_save_r1:   .long 0

_handler_end:
