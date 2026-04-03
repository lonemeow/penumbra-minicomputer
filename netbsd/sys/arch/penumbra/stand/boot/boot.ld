/*
 * boot.ld — Linker script for Penumbra stage 1 bootloader
 *
 * Loaded by ROM into base RAM at 0x00001000 (page 1, after vector
 * table and boot data on page 0).  Stage 1 has up to ~1 MB from
 * the partition gap.
 */

ENTRY(main)

SECTIONS
{
    . = 0x00001000;

    .text : {
        *(.text .text.*)
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

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
