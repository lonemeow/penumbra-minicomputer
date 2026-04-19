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

    .init_array : {
        _init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array .ctors))
        _init_array_end = .;
    }

    .fini_array : {
        _fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array .dtors))
        _fini_array_end = .;
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
