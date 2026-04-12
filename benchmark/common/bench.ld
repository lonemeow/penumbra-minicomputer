/*
 * bench.ld — Linker script for Penumbra bare-metal benchmarks (PIE ELF)
 *
 * Linked as a PIE (--pie --image-base=0) so the ROM boot command can
 * load the binary at any available RAM address.  The CRT stub in
 * crt0.S handles self-relocation before calling bench_main().
 */

ENTRY(_start)

SECTIONS
{
    . = SIZEOF_HEADERS;

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

    .dynamic : {
        *(.dynamic)
    }

    .rel.dyn : {
        *(.rel.dyn)
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
    }
}
