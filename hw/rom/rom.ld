/* rom.ld — Penumbra boot ROM linker script
 *
 * ROM lives at 0xFFFF_E000 (8 KB).  CPU boots at this address.
 * RAM starts at 0x0000_0000 (16 MB).  Stack grows down from top of RAM.
 *
 * Everything lands in ROM — no writable .data/.bss support yet.
 * (A ROM monitor doesn't need mutable globals; locals live on the stack.)
 */

ENTRY(_start)

MEMORY
{
    ROM (rx)  : ORIGIN = 0xFFFFE000, LENGTH = 8K
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 16M
}

SECTIONS
{
    /* Code and read-only data in ROM */
    .text : {
        *(.text._start)        /* entry point first */
        *(.text .text.*)
    } > ROM

    .rodata : {
        *(.rodata .rodata.*)
    } > ROM

    /* Ensure we don't silently overflow the ROM */
    ASSERT(. <= 0xFFFFFFFF, "ROM overflow: code + rodata exceed 8 KB")

    /* Discard everything else — no writable sections in ROM builds */
    /DISCARD/ : {
        *(.data .data.* .bss .bss.* .sbss .sbss.* COMMON)
        *(.comment .note.* .eh_frame .ARM.*)
    }
}
