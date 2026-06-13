/* test/compiler/harness/test-pic.ld — like test.ld, with a .got section */
OUTPUT_FORMAT("elf32-penumbra")
ENTRY(_start)

SECTIONS
{
    . = 0x00001000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }

    /* PIC leg: GOT is resolved by lld at link time (static,
       non-PIE, fixed load address), so the flat binary carries
       fully-populated GOT entries and needs no runtime relocator. */
    .got : {
        *(.got)
        *(.got.plt)
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

    .tdata : {
        _tdata_start = .;
        *(.tdata .tdata.*)
        _tdata_end = .;
    }

    /* Single-threaded bare-metal: .tbss lives immediately after
       .tdata so one contiguous TP-relative block covers both.
       Zero-init of .tbss is the caller's responsibility — crt0
       does not currently clear it (no torture test exercises it
       yet; add a separate clear loop when one does). */
    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
        _tbss_end = .;
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
