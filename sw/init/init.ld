/* Linker script for minimal init — single PT_LOAD + PT_NOTE */
ENTRY(_start)

PHDRS {
    note PT_NOTE FLAGS(4);      /* r-- */
    load PT_LOAD FLAGS(7);      /* rwx — single segment for tiny binary */
}

SECTIONS {
    . = 0x1000 + SIZEOF_HEADERS;

    .text : { *(.text .text.*) } :load
    .rodata : { *(.rodata .rodata.*) } :load
    .note.netbsd.ident : { *(.note.netbsd.ident) } :note

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.gnu.stack) }
}
