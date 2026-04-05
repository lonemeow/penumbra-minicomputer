/*
 * boot.ld — Linker script for Penumbra boot loader (PIE ELF)
 *
 * Linked as a PIE (--pie --image-base=0) so the ROM can load it
 * at any available RAM address.  The CRT stub in crt0.s handles
 * self-relocation before calling main().
 */

ENTRY(_start)

SECTIONS
{
    . = SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
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
    }
}
