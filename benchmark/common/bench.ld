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

    /* Page-align data so global addresses depend only on the page size,
     * not the (codegen-dependent) size of .text.  4096 is a multiple of
     * the 1 KB D-cache, so cache-set mappings stay identical across builds
     * and benchmarks don't swing on layout jitter.  (See dhry.ld.) */
    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    .dynamic : {
        *(.dynamic)
    }

    .rel.dyn : {
        *(.rel.dyn)
    }

    /* Bulk payloads a program links in — an audio clip is megabytes.
     * Kept past .data because crt0 reaches its PIE address slots with
     * a 16-bit PC-relative displacement, and anything large sitting
     * between .text and .data puts them out of range.  C code reaches
     * its own globals through the GOT, so .bss is free to move. */
    . = ALIGN(4096);
    .payload : {
        *(.payload .payload.*)
    }

    . = ALIGN(4096);
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
