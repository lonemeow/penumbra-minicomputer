/*
 * dhry_crt0.S — Bare-metal startup for the static-link Dhrystone build.
 *
 * Differs from common/crt0.S in two ways:
 *   1. The image is NOT PIE.  It is linked statically at virtual
 *      address 0x80000000, so global accesses compile down to the
 *      classic 2-instruction LUI+LDW sequence — no GOT indirection.
 *   2. The TLB miss handler is range-aware.  Faults inside the
 *      [DHRY_VBASE, DHRY_VBASE+image_size) window are mapped to
 *      PA = VA - DHRY_VBASE + load_base (the physical address ROM
 *      chose); faults below DHRY_VBASE stay identity-mapped (low
 *      RAM, page-0 vectors, stack); faults at >= 0xFF000000 stay
 *      identity-mapped uncached (MMIO).
 *
 * Entry state (from ROM boot command):
 *   R1 = physical pointer to boot data
 *   Supervisor mode, MMU off, interrupts off
 *
 * Startup phases:
 *   1. Compute load_bias = runtime_PC − linktime_PC
 *      (= load_base − DHRY_VBASE, possibly negative i.e. wrapping)
 *   2. Copy TLB miss + IRQ handlers to page 0x1000 and patch the
 *      load_bias scratch slot inside the copy
 *   3. Install vector table entries, pin handler page + page 0
 *   4. Enable MMU (image is now reachable at its linked VA)
 *   5. Enable caches
 *   6. Set up stack, call bench_main(bootdata) at fixed VA
 */

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

/* ── Cache sysregs ────────────────────────────────────────────── */
#define SYSDEV_L1_DCACHE    2
#define SYSDEV_L1_ICACHE    3
#define SYSDEV_L2_CACHE     9
#define CACHE_INFO          0
#define CACHE_CTRL          1
#define CACHE_CTRL_ENABLE   1

/* ── PTE flags ────────────────────────────────────────────────── */
#define PTE_KERNEL          0xBD    /* V|C|R|W|X|G */
#define PTE_KERNEL_NC       0x99    /* V|R|W|G (uncached, for MMIO) */

/* ── Pinned TLB ───────────────────────────────────────────────── */
#define TLB_PINNED          0x40

/* ── Fixed addresses ──────────────────────────────────────────── */
#define HANDLER_PAGE        0x1000
#define TIMER_BASE_ADDR     0x38         /* accumulated ticks, page 0 */
#define STACK_TOP           0x00100000   /* 1 MB — identity-mapped */
#define DHRY_VBASE          0x80000000   /* must match dhry.ld */


/* ═════════════════════════════════════════════════════════════════
 * Phase 1: Entry + compute load_bias
 *
 * We are executing at the physical address ROM picked, but the
 * image was linked for DHRY_VBASE.  Any PC-relative reference to
 * a same-image symbol at runtime yields the runtime address
 * automatically (PC + (link_sym − link_pc) = sym + load_bias).
 *
 * To find load_bias itself we read a .data slot whose stored value
 * is the slot's own *linktime* address; subtracting that from the
 * slot's runtime address (which we get via PC-relative addressing)
 * gives the bias.
 * ═════════════════════════════════════════════════════════════════ */

        .section .text._start,"ax",@progbits
        .globl  _start
        .type   _start,@function

_start:
        mov     r5, r1                  /* save bootdata pointer */
        li      sp, 0x00080000          /* temp stack at 512 KB (identity-mapped) */

        /* r4 = load_bias = runtime(.Lselfaddr) − linktime(.Lselfaddr) */
        mov     r4, pc
        add     r4, #8
        add     r4, #%pcrel(.Lselfaddr)  /* r4 = runtime address of slot */
        ldw     r6, [r4]                 /* r6 = linktime address of slot */
        sub     r4, r6                   /* r4 = load_bias */

/* ═════════════════════════════════════════════════════════════════
 * Phase 2: Copy handlers to page 0x1000 and patch load_bias
 *
 * Handler block uses PC-relative scratch addressing throughout, so
 * a verbatim word-for-word copy works at any page-aligned target.
 * After copy we patch a single scratch word inside the copied block
 * with the load_bias value the miss handler needs at runtime.
 * ═════════════════════════════════════════════════════════════════ */

        /* Clear timer overflow accumulator */
        stw     r0, [r0 + TIMER_BASE_ADDR]

        /* r1 = runtime &_handler_start, r2 = runtime &_handler_end */
        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(_handler_start)
        mov     r2, pc
        add     r2, #8
        add     r2, #%pcrel(_handler_end)

        /* Word-by-word copy to HANDLER_PAGE */
        li      r3, HANDLER_PAGE
.Lcopy_loop:
        cmp     r1, r2
        bhs     .Lcopy_done
        ldw     r6, [r1]
        stw     r6, [r3]
        add     r1, #4
        add     r3, #4
        b       .Lcopy_loop
.Lcopy_done:

        /* Patch the load_bias slot inside the copy.
         * Offset of slot from _handler_start is a link-time constant. */
        li      r1, HANDLER_PAGE + (_rmh_load_bias - _handler_start)
        stw     r4, [r1]

        /* Install vector table entries.
         * Target = HANDLER_PAGE + (handler_symbol − _handler_start). */
        li      r1, HANDLER_PAGE + (_bench_tlb_miss - _handler_start)
        stw     r1, [r0 + 8]            /* vector[2] = TLB miss */

        li      r1, HANDLER_PAGE + (_bench_irq - _handler_start)
        stw     r1, [r0 + 4]            /* vector[1] = IRQ */

/* ═════════════════════════════════════════════════════════════════
 * Phase 3: Pin TLB slots
 *
 * Slot 0: handler page (0x1000) — TLB miss + IRQ code
 * Slot 1: page 0 (0x0000) — vectors, bootdata, overflow counter
 *
 * Both identity-mapped (VA = PA) so handlers can run without
 * nested TLB misses.  The dhrystone window at DHRY_VBASE is
 * left to the miss handler.
 * ═════════════════════════════════════════════════════════════════ */

        /* Slot 0: handler page at PA 0x1000 */
        lli     r1, #TLB_PINNED
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX

        lli     r1, #0x100              /* VPN = (0x1 << 8) | ASID=0 */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_VPN

        lli     r1, #0x10BD             /* PTE = PA 0x1000 | PTE_KERNEL */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        /* Slot 1: page 0 at PA 0x0000 */
        lli     r1, #(TLB_PINNED | 1)
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX

        lli     r1, #0                  /* VPN = 0 */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_VPN

        lli     r1, #PTE_KERNEL         /* PTE = PA 0 | PTE_KERNEL */
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

/* ═════════════════════════════════════════════════════════════════
 * Phase 4: Pin a TLB entry for the current code page, then enable MMU
 *
 * After MMUCR.M=1, instruction fetches go through the TLB.  We are
 * still running at the physical load address, so this entry maps
 * VA == PA for the current page (one extra pinned slot).  Once the
 * very next instruction (a fixed-VA branch) lands, fetches come
 * from the DHRY_VBASE range and the miss handler refills as needed.
 * ═════════════════════════════════════════════════════════════════ */

        /* Slot 2: identity-map current physical code page */
        lli     r1, #(TLB_PINNED | 2)
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_INDEX

        mov     r1, pc
        shr     r1, #12                 /* PPN of current PC */

        mov     r2, r1
        shl     r2, #8                  /* VPN<<8 | ASID=0 */
        wrsys   r2, #SYSDEV_MMU, #MMU_TLB_VPN

        shl     r1, #12                 /* PA page base */
        lli     r2, #PTE_KERNEL
        or      r1, r2
        wrsys   r1, #SYSDEV_MMU, #MMU_TLB_PTE

        /* Turn the MMU on */
        lli     r1, #MMUCR_M
        wrsys   r1, #SYSDEV_MMU, #MMU_MMUCR

        /* Long jump to fixed-VA continuation.  Until this branch
         * resolves, we are still fetching from the identity-pinned
         * physical page.  The target is an absolute (fixed-VA)
         * address, so the very next fetch goes through the miss
         * handler, which refills via the biased PA mapping. */
        li      r1, .Lin_vaddr
        jmp     r1

.Lin_vaddr:
        /* Enable caches.  PTE.C declares pages cacheable but the
         * cache modules themselves are disabled at reset and pass
         * everything through to memory until ENABLE is set.  Valid
         * bits are cleared at reset, so no invalidate needed. */
        lli     r1, #CACHE_CTRL_ENABLE
        wrsys   r1, #SYSDEV_L1_DCACHE, #CACHE_CTRL
        wrsys   r1, #SYSDEV_L1_ICACHE, #CACHE_CTRL

        /* Enable L2 if present.  INFO=0 means the device id is
         * unmapped on this bitstream; skip the enable write. */
        rdsys   r2, #SYSDEV_L2_CACHE, #CACHE_INFO
        cmp     r2, r0
        beq     .Lno_l2
        lli     r1, #CACHE_CTRL_ENABLE
        wrsys   r1, #SYSDEV_L2_CACHE, #CACHE_CTRL
.Lno_l2:

/* ═════════════════════════════════════════════════════════════════
 * Phase 5: Set stack + call bench_main
 * ═════════════════════════════════════════════════════════════════ */

        li      sp, STACK_TOP
        mov     r1, r5                  /* bootdata pointer */
        bl      bench_main
        break                           /* halt CPU */

        .size   _start, . - _start


/* ── Self-address slot (.data, NOT relocated — value is link-time) ─
 *
 * The stored long is .Lselfaddr's *linktime* address (a constant
 * the assembler/linker computes).  At runtime PC-relative access
 * gives us the slot's runtime address; subtracting yields the bias.
 */
        .data
        .align  4
.Lselfaddr:
        .long   .Lselfaddr


/* ═════════════════════════════════════════════════════════════════
 * Handler code — copied to page 0x1000 at startup.
 *
 * Block layout (all PC-relative addressing inside):
 *   _handler_start
 *     _bench_tlb_miss   (range-aware refill)
 *     _bench_irq        (timer overflow accumulator)
 *     scratch words     (register saves, way counter, load_bias)
 *   _handler_end
 * ═════════════════════════════════════════════════════════════════ */

        .section .handlers,"ax",@progbits
        .balign 4
        .globl  _handler_start
        .globl  _handler_end

_handler_start:

/* ── Range-aware TLB miss handler ──────────────────────────────
 *
 * Three address ranges, three mapping policies:
 *
 *   VA >= 0xFF000000              → uncached identity (MMIO)
 *   VA >= DHRY_VBASE (0x80000000) → cached, PA = VA + load_bias
 *   otherwise                      → cached identity (low RAM)
 *
 * Uses alternating-way replacement (bit 5 of TLB_INDEX).
 * ────────────────────────────────────────────────────────────── */
        .globl  _bench_tlb_miss
_bench_tlb_miss:
        stw     r1, [pc + _rmh_save_r1 - .]
        stw     r2, [pc + _rmh_save_r2 - .]
        stw     r3, [pc + _rmh_save_r3 - .]

        /* r1 = fault VA */
        rdsys   r1, #SYSDEV_MMU, #MMU_FAULT_ADDR

        /* r2 = VPN, r3 = TLB_INDEX (set ∈ VPN[4:0] | way bit) */
        mov     r2, r1
        shr     r2, #12
        mov     r3, r2
        and     r3, #0x1F
        ldw     r1, [pc + _rmh_way_ctr - .]
        or      r3, r1
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_INDEX

        /* TLB_VPN = (VPN << 8) | ASID=0 */
        mov     r3, r2
        shl     r3, #8
        wrsys   r3, #SYSDEV_MMU, #MMU_TLB_VPN

        /* ── Compose TLB_PTE ────────────────────────────────────
         * On entry: r1 = fault VA, r2 = VPN (= fault VA >> 12).
         * On exit:  r3 = TLB_PTE value (PA page base | PTE flags).
         * Then fall through to the WRSYS + restore below.
         *
         * Three cases (see header comment for ranges).  The
         * load_bias scratch slot holds (load_base − DHRY_VBASE)
         * as a wrap-around uint32_t; adding it to a high-half VA
         * yields the physical address.
         *
         * Hint: the existing common/crt0.S handler is two-case
         * (identity vs uncached) — extend that pattern with a
         * third branch that biases the PA before OR'ing the
         * cached PTE flags.  Use r3 to build the PA-page-base,
         * then OR/add the chosen PTE flag word into r3.
         */
        /* Classify on VA[31:24] (top byte of VPN).  Three policies:
         *   top >= 0xFF → uncached identity (MMIO)
         *   top >= 0x80 → cached biased    (dhrystone window)
         *   otherwise   → cached identity  (low RAM, stack)
         *
         * Common prefix: r3 = VPN << 12 = VA page base.  For both
         * identity arms this is already the PA page base; only the
         * biased arm needs to add load_bias (which is page-aligned,
         * so it doesn't disturb the low 12 bits we're about to OR
         * with the PTE flag word).  Avoids depending on the fault
         * VA in r1, which the way-counter load above has clobbered.
         */
        mov     r3, r2
        shl     r3, #12                 /* r3 = VA page base */

        mov     r1, r2
        shr     r1, #12                 /* r1 = VA[31:24] */
        cmp     r1, #0xFF
        bhs     .Luncached
        cmp     r1, #0x80
        bhs     .Lbiased

        /* Identity, cached */
        add     r3, #PTE_KERNEL
        b       .Lwrite_pte

.Lbiased:
        /* Cached, with load_bias (load_bias is page-aligned) */
        ldw     r1, [pc + _rmh_load_bias - .]
        add     r3, r1                  /* r3 = PA page base */
        add     r3, #PTE_KERNEL
        b       .Lwrite_pte

.Luncached:
        /* Identity, uncached (MMIO) */
        add     r3, #PTE_KERNEL_NC

.Lwrite_pte:
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


/* ── IRQ handler: timer tick accumulation (unchanged from common) ─ */
        .balign 4
        .globl  _bench_irq
_bench_irq:
        stw     r1, [pc + _irq_save_r1 - .]

        /* Clear timer UDF (write-1-to-clear) */
        lli     r1, #1
        wrsys   r1, #SYSDEV_TIMER, #TM_STATUS

        /* Accumulate one period (0xFFFF = reload + 1) */
        ldw     r1, [r0 + TIMER_BASE_ADDR]
        add     r1, #0xFFFF
        stw     r1, [r0 + TIMER_BASE_ADDR]

        ldw     r1, [pc + _irq_save_r1 - .]
        eret


/* ── Scratch words (contiguous with code for PC-relative access) ─
 *
 * _rmh_load_bias is patched by phase 2 of _start before the MMU
 * is enabled, so the miss handler sees the correct value on its
 * very first invocation.
 */
        .balign 4
_rmh_save_r1:   .long 0
_rmh_save_r2:   .long 0
_rmh_save_r3:   .long 0
_rmh_way_ctr:   .long 0
_rmh_load_bias: .long 0
_irq_save_r1:   .long 0

_handler_end:
