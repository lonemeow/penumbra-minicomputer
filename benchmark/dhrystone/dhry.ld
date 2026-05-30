/*
 * dhry.ld — Linker script for the static-link Dhrystone build.
 *
 * Linked at a fixed virtual address (0x80000000) so clang emits
 * the classic 2-instruction LUI+LDW global-access sequence rather
 * than the 4-instruction GOT indirection PIE codegen produces.
 *
 * The ROM ELF loader copies bytes to its chosen physical base
 * (load_base) and relocates e_entry against the lowest p_vaddr —
 * so PC-relative branches just work; only absolute global addresses
 * need the runtime VA→PA mapping that dhry_crt0.S installs.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x80000000 + SIZEOF_HEADERS;

    .text : {
        *(.text._start)        /* entry point first */
        *(.text .text.*)
    }

    .handlers : {
        *(.handlers .handlers.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    _end = .;
    PROVIDE(end = .);

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.dynamic)
        *(.rel.dyn)
        *(.rela.dyn)
        *(.hash)
        *(.dynsym)
        *(.dynstr)
    }
}
