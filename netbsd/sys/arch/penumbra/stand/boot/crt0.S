/*
 * crt0.s — PIE self-relocator and C entry for Penumbra boot loader
 *
 * This runs as the very first code after the ROM jumps to us.
 * Before any C code can execute, we must process R_PENUMBRA_RELATIVE
 * dynamic relocations to fix up absolute addresses (function
 * pointers, global data) for our actual load address.
 *
 * Entry state (from ROM):
 *   R1 = physical pointer to boot data
 *   PC = entry point (wherever ROM loaded us)
 *   Supervisor mode, MMU off, interrupts off
 *
 * Algorithm:
 *   1. Find _DYNAMIC at runtime via PC-relative addressing
 *   2. Load _DYNAMIC's link-time address via LLI/LUI
 *   3. bias = runtime_DYNAMIC - linktime_DYNAMIC
 *   4. Walk .dynamic to find DT_REL (addr) and DT_RELSZ (size)
 *   5. For each R_PENUMBRA_RELATIVE: *(offset + bias) += bias
 *   6. Call main(bootdata)
 */

#define DT_NULL              0
#define DT_REL               17
#define DT_RELSZ             18
#define R_PENUMBRA_RELATIVE  8

    .text
    .globl _start
_start:
    /* Save boot data pointer in callee-saved register */
    mov  r5, r1

    /* Set up a proper stack just below where loader is loaded */
    lli  sp, #0xB000

    /*
     * Compute load bias = runtime _DYNAMIC - linktime _DYNAMIC.
     *
     * Runtime address: PC-relative (mov pc + add %pcrel), no
     * absolute relocation needed.
     *
     * Link-time address: stored as a .word in .data, which the
     * linker fills via R_PENUMBRA_32 → becomes R_PENUMBRA_RELATIVE
     * in the PIE.  Before the relocator runs, the word still
     * contains the original link-time value.  We read it here,
     * compute bias, then the reloc loop patches it (harmless).
     */
    /*
     * r2 = runtime address of _DYNAMIC (PC-relative).
     * mov rN, pc reads the CURRENT instruction's address (PC is
     * not yet advanced).  The %pcrel on the add is relative to
     * the add instruction itself, so we need +4 to bridge the
     * gap between the mov and the add.
     */
    mov  r2, pc
    add  r2, #8
    add  r2, #%pcrel(_DYNAMIC)

    /* r3 = runtime address of .Ldynamic_linkaddr */
    mov  r3, pc
    add  r3, #8
    add  r3, #%pcrel(.Ldynamic_linkaddr)
    /* r3 = link-time _DYNAMIC (read from .data, pre-relocation) */
    ldw  r3, [r3]

    /* r4 = load bias */
    mov  r4, r2
    sub  r4, r3

    /* If bias is zero, skip relocation (loaded at link address) */
    cmp  r4, #0
    beq  .Lreloc_done

    /*
     * Walk .dynamic table to find DT_REL and DT_RELSZ.
     * r2 = pointer into .dynamic (runtime address)
     * r6 = DT_REL value (link-time address of .rel.dyn)
     * r7 = DT_RELSZ value (size in bytes)
     */
    lli  r6, #0
    lli  r7, #0

.Ldyn_loop:
    ldw  r8, [r2]           /* d_tag */
    cmp  r8, #DT_NULL
    beq  .Ldyn_done
    ldw  r9, [r2 + 4]       /* d_val */
    cmp  r8, #DT_REL
    beq  .Lfound_rel
    cmp  r8, #DT_RELSZ
    beq  .Lfound_relsz
    add  r2, #8
    b    .Ldyn_loop

.Lfound_rel:
    mov  r6, r9
    add  r2, #8
    b    .Ldyn_loop

.Lfound_relsz:
    mov  r7, r9
    add  r2, #8
    b    .Ldyn_loop

.Ldyn_done:
    /* r6 = link-time .rel.dyn address, adjust to runtime */
    add  r6, r4             /* r6 = runtime &.rel.dyn */
    /* r7 = size; compute end pointer */
    add  r7, r6             /* r7 = end of .rel.dyn */

    /*
     * Process Elf32_Rel entries (8 bytes each):
     *   [0] r_offset  — location to patch (link-time)
     *   [4] r_info    — (sym << 8) | type
     *
     * For R_PENUMBRA_RELATIVE (type 8):
     *   runtime_addr = r_offset + bias
     *   *runtime_addr += bias
     */
.Lrel_loop:
    cmp  r6, r7
    bhs  .Lreloc_done
    ldw  r8, [r6]           /* r_offset */
    ldw  r9, [r6 + 4]       /* r_info */
    and  r9, #0xFF          /* type = r_info & 0xFF */
    cmp  r9, #R_PENUMBRA_RELATIVE
    bne  .Lrel_next

    /* Patch this location */
    add  r8, r4             /* r8 = runtime address */
    ldw  r9, [r8]           /* current value (link-time addr) */
    add  r9, r4             /* add bias */
    stw  r9, [r8]           /* write back */

.Lrel_next:
    add  r6, #8
    b    .Lrel_loop

.Lreloc_done:
    /* Restore boot data pointer and call main */
    mov  r1, r5
    bl   main
    break

    /*
     * Link-time address of _DYNAMIC, stored as a plain .word.
     * In the PIE ELF this becomes an R_PENUMBRA_RELATIVE reloc,
     * but BEFORE the relocator runs, the word contains the
     * original link-time value — exactly what we need to
     * compute the bias.  The reloc loop later patches it to
     * the runtime value (harmless, we're done with it by then).
     */
    .data
    .align 4
.Ldynamic_linkaddr:
    .long _DYNAMIC
