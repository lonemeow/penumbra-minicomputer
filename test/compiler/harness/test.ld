/* test/compiler/harness/test.ld */
OUTPUT_FORMAT("elf32-penumbra")
ENTRY(_start)

SECTIONS
{
    . = 0x00001000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _bss_end = .;
    }

    /* Set end of RAM for stack if needed by symbols */
    _end = .;
}
