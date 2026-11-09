/*
 * crt0_static.S — Bare-metal startup for the static-link benchmarks.
 *
 * Differs from crt0.S in that the image is NOT PIE.  It is linked at
 * virtual address BENCH_VBASE (0x80000000), so global accesses compile
 * down to the classic 2-instruction LUI+LDW sequence — no GOT
 * indirection.  The harness's miss handler maps that window to where
 * the ROM loaded the image, PA = VA + load_bias (mmu.h).
 *
 * Entry state (from ROM boot command):
 *   R1 = physical pointer to boot data
 *   Supervisor mode, MMU off, interrupts off
 *
 * Startup phases:
 *   1. Compute load_bias = runtime_PC − linktime_PC
 *      (= load_base − BENCH_VBASE, possibly negative i.e. wrapping)
 *   2. bench_mmu_start (mmu.S): TLB, handlers, translation and caches
 *   3. Jump to the linked VA, set up the stack (in .bss), call
 *      bench_main(bootdata)
 */

/* ── Stack ────────────────────────────────────────────────────────
 * The stack is part of the image's .bss, above BENCH_VBASE, so the ROM
 * allocates it with the program and the miss handler maps it with the
 * load bias. */
#define BENCH_STACK_BYTES   16384


/* ═════════════════════════════════════════════════════════════════
 * Phase 1: Entry + compute load_bias
 *
 * We are executing at the physical address ROM picked, but the
 * image was linked for BENCH_VBASE.  Any PC-relative reference to
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

        /* r4 = load_bias = runtime(.Lselfaddr) − linktime(.Lselfaddr) */
        mov     r4, pc
        add     r4, #8
        add     r4, #%pcrel(.Lselfaddr)  /* r4 = runtime address of slot */
        ldw     r6, [r4]                 /* r6 = linktime address of slot */
        sub     r4, r6                   /* r4 = load_bias */

/* ═════════════════════════════════════════════════════════════════
 * Phase 2: Translation, through the harness (mmu.S)
 *
 * PC-relative, the handler block's address is its runtime one.  The
 * harness returns with translation on, still at the physical address,
 * which its handler maps identically; the absolute jump then lands in
 * the BENCH_VBASE window, mapped with the bias.
 * ═════════════════════════════════════════════════════════════════ */

        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(_handler_start)
        mov     r2, r4
        bl      bench_mmu_start         /* keeps R5 */

        li      r1, .Lin_vaddr
        jmp     r1
.Lin_vaddr:

/* ═════════════════════════════════════════════════════════════════
 * Phase 3: Set stack + call bench_main
 * ═════════════════════════════════════════════════════════════════ */

        li      sp, _bench_stack_top
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


/* ── Stack, allocated with the image ─────────────────────────────── */
        .section .bss.stack,"aw",@nobits
        .balign 16
_bench_stack:
        .space  BENCH_STACK_BYTES
_bench_stack_top:
