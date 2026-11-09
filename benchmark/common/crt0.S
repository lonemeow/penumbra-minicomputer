/*
 * crt0.S — Bare-metal benchmark startup for Penumbra
 *
 * Entry state (from ROM boot command):
 *   R1 = physical pointer to boot data
 *   Supervisor mode, MMU off, interrupts off
 *
 * Startup phases:
 *   1. PIE self-relocation (process R_PENUMBRA_RELATIVE)
 *   2. bench_mmu_start (mmu.S): TLB, handlers, translation and caches
 *   3. Set up the stack (in .bss), call bench_main(bootdata)
 */

/* ── ELF dynamic section tags ─────────────────────────────────── */
#define DT_NULL             0
#define DT_RELA             7
#define DT_RELASZ           8
#define R_PENUMBRA_RELATIVE 8

/* ── Stack ────────────────────────────────────────────────────────
 * The stack is part of the image's .bss, so the ROM allocates it with
 * the rest of the program. */
#define BENCH_STACK_BYTES   16384


/* ═════════════════════════════════════════════════════════════════
 * Phase 1: Entry + PIE self-relocation
 *
 * Same algorithm as the bootloader (stand/boot/crt0.S):
 * compute load bias from _DYNAMIC, walk .rela.dyn, patch
 * R_PENUMBRA_RELATIVE entries.
 * ═════════════════════════════════════════════════════════════════ */

        .section .text._start,"ax",@progbits
        .globl  _start
        .type   _start,@function

_start:
        mov     r5, r1                  /* save bootdata pointer */

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

        /* Walk .dynamic for DT_RELA and DT_RELASZ */
        lli     r6, #0
        lli     r7, #0

.Ldyn_loop:
        ldw     r8, [r2]
        cmp     r8, #DT_NULL
        beq     .Ldyn_done
        ldw     r9, [r2 + 4]
        cmp     r8, #DT_RELA
        beq     .Lfound_rela
        cmp     r8, #DT_RELASZ
        beq     .Lfound_relasz
        add     r2, #8
        b       .Ldyn_loop

.Lfound_rela:
        mov     r6, r9
        add     r2, #8
        b       .Ldyn_loop

.Lfound_relasz:
        mov     r7, r9
        add     r2, #8
        b       .Ldyn_loop

.Ldyn_done:
        add     r6, r4                  /* runtime &.rela.dyn */
        add     r7, r6                  /* end of .rela.dyn */

.Lrela_loop:
        cmp     r6, r7
        bhs     .Lreloc_done
        ldw     r8, [r6]               /* r_offset */
        ldw     r9, [r6 + 4]            /* r_info */
        and     r9, #0xFF              /* type */
        cmp     r9, #R_PENUMBRA_RELATIVE
        bne     .Lrela_next
        add     r8, r4                  /* runtime address */
        ldw     r9, [r6 + 8]            /* r_addend */
        add     r9, r4                  /* bias + addend */
        stw     r9, [r8]
.Lrela_next:
        add     r6, #12
        b       .Lrela_loop

.Lreloc_done:

/* ═════════════════════════════════════════════════════════════════
 * Phase 2: Translation, through the harness (mmu.S)
 *
 * A PIE image runs where it was loaded, so its load bias for the
 * BENCH_VBASE window is zero.  The handler block's runtime address
 * comes from a .data slot the relocator patched.
 * ═════════════════════════════════════════════════════════════════ */

        mov     r1, pc
        add     r1, #8
        add     r1, #%pcrel(.Laddr_handler_start)
        ldw     r1, [r1]
        mov     r2, r0
        bl      bench_mmu_start         /* keeps R5 */

/* ═════════════════════════════════════════════════════════════════
 * Phase 3: Set stack + call bench_main
 * ═════════════════════════════════════════════════════════════════ */

        /* The stack's top, from a .data slot the relocator patched: a
         * direct PC-relative reference cannot reach past a large image. */
        mov     r2, pc
        add     r2, #8
        add     r2, #%pcrel(.Laddr_stack_top)
        ldw     r2, [r2]
        mov     sp, r2
        mov     r1, r5                  /* bootdata pointer */
        bl      bench_main
        break                           /* halt CPU */

        .size   _start, . - _start


/* ── PIE address slots (.data, patched by self-relocator) ──────── */
        .data
        .align  4
.Ldynamic_linkaddr:
        .long   _DYNAMIC
.Laddr_stack_top:
        .long   _bench_stack_top
.Laddr_handler_start:
        .long   _handler_start


/* ── Stack, allocated with the image ─────────────────────────────── */
        .section .bss.stack,"aw",@nobits
        .balign 16
_bench_stack:
        .space  BENCH_STACK_BYTES
_bench_stack_top:
