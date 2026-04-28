// ram_check.s — pre-stack RAM/SDRAM controller diagnostic
//
// Validates that the memory controller behaves correctly before the
// C runtime sets up its stack in low RAM (the very RAM we are about
// to test).  This is permanent boot code — it should remain enabled
// across SDRAM controller revisions and provide early visibility
// into regressions.
//
// Output:
//   "\r\nRAM: OK\r\n"            All controller checks passed.
//   "\r\nRAM: FAIL <step> got <hex>\r\n"
//                                A check failed; <step> identifies
//                                which, <hex> is the offending
//                                32-bit readback.
//
// Checks (controller-correctness, not chip-level data integrity):
//
//   1A — Round-trip: write a value to one address, read it back.
//        Catches: writes never landing, bus floating with capacitive
//        retention of last driven value, reads stuck at a constant.
//
//   1B — Overwrite: write a different value to the same address,
//        read it back.  Catches: write-once-only behaviour,
//        first-write capture latches that never reload.
//
//   2x — Consecutive multi-word write/read at four sequential
//        addresses (x = 0..3 picks which one mismatched).
//        Catches: later writes corrupting earlier ones (auto-
//        precharge / refresh interaction), address-increment bugs
//        in the controller, FIFO ordering failures in future burst
//        paths.  Each word holds a distinct value so a misordered
//        read shows up as the wrong index, not just "wrong data".
//
//   3B / 3H — Sub-word integrity: a byte (3B) or halfword (3H)
//        write must affect only the targeted lanes and leave
//        neighbours untouched.  Catches: missing/incorrect DQM
//        gating, broken read-modify-write paths, byte-enable
//        decode bugs.  Easy bug to introduce in a controller
//        rewrite, very visible to userland (memcpy of unaligned
//        regions silently corrupts data otherwise).
//
// Platform constraint: the boot environment guarantees only 2 pages
// (8 KB) of RAM at address 0.  All test addresses stay within
// [0x100, 0x200) — well below the stack top at 0x2000 and far away
// from the vector page at 0.
//
// Stack discipline: this routine runs before any stack exists.  The
// outer LR is saved in r9 (not pushed to memory).  _dump_r5 and
// _early_puts both stash their own caller LR in r10; safe because
// they never nest (we always finish a puts before calling dump).
//
// On entry to _ram_check, the caller (crt0) must have:
//   r11 = LSR_THRE mask (0x20)
//   r12 = UART_BASE     (0xFF00_0000)

        .section .text,"ax",@progbits

.equ UART_LSR,  0x14

// ────────────────────────────────────────────────────────────────
// _ram_check — print "\r\nRAM: <result>\r\n".
//   On success:           "\r\nRAM: OK\r\n"
//   On any check failure: "\r\nRAM: FAIL <step> got <hex>\r\n"
//
// On entry: r11 = LSR_THRE, r12 = UART_BASE.
// Saves outer LR in r9 (r10 used by _dump_r5 / _early_puts).
// Clobbers r1-r8.
// ────────────────────────────────────────────────────────────────
        .globl  _ram_check
        .type   _ram_check,@function
_ram_check:
        mov   r9, r13              // save caller LR

        li    r1, .Lstr_ram_header
        bl    _early_puts

        // ── Check 1A: round-trip ─────────────────────────────────
        li    r3, 0x100
        li    r4, 0xDEADBEEF
        stw   r4, [r3]
        ldw   r5, [r3]
        cmp   r5, r4
        beq   _check_1B
        lli   r6, '1'
        lli   r7, 'A'
        b     _ram_fail

_check_1B:
        // ── Check 1B: overwrite ──────────────────────────────────
        li    r4, 0x55AA55AA
        stw   r4, [r3]
        ldw   r5, [r3]
        cmp   r5, r4
        beq   _check_2
        lli   r6, '1'
        lli   r7, 'B'
        b     _ram_fail

_check_2:
        // ── Check 2: consecutive 4-word write/read ──────────────
        // Distinct values per slot so a misread shows up as the
        // wrong index rather than just "wrong data".
        li    r3, 0x100
        li    r4, 0xC0DE0000
        stw   r4, [r3]
        add   r4, 1
        stw   r4, [r3 + 4]
        add   r4, 1
        stw   r4, [r3 + 8]
        add   r4, 1
        stw   r4, [r3 + 12]

        lli   r6, '2'              // group prefix
        li    r4, 0xC0DE0000       // expected, increments per slot

        ldw   r5, [r3]
        cmp   r5, r4
        beq   _check_2_1
        lli   r7, '0';  b _ram_fail
_check_2_1:
        add   r4, 1
        ldw   r5, [r3 + 4]
        cmp   r5, r4
        beq   _check_2_2
        lli   r7, '1';  b _ram_fail
_check_2_2:
        add   r4, 1
        ldw   r5, [r3 + 8]
        cmp   r5, r4
        beq   _check_2_3
        lli   r7, '2';  b _ram_fail
_check_2_3:
        add   r4, 1
        ldw   r5, [r3 + 12]
        cmp   r5, r4
        beq   _check_3
        lli   r7, '3';  b _ram_fail

_check_3:
        // ── Check 3: sub-word integrity at 0x180 ────────────────
        // Stage 3B: write a sentinel 32-bit word, overwrite a single
        // byte, verify the remaining three bytes are unchanged.
        // Stage 3H: also overwrite a halfword and re-verify the
        // remaining bytes/halfword.

        li    r3, 0x180
        li    r4, 0xCAFEFACE
        stw   r4, [r3]

        li    r4, 0x22
        stb   r4, [r3 + 1]
        li    r6, 0xCAFE22CE
        ldw   r5, [r3]
        cmp   r5, r6
        beq   _check_3_1
        lli   r6, '3'
        lli   r7, 'B'
        b     _ram_fail
_check_3_1:
        li    r4, 0x1111
        sth   r4, [r3]
        li    r6, 0xCAFE1111
        ldw   r5, [r3]
        cmp   r5, r6
        beq   _ram_pass
        lli   r6, '3'
        lli   r7, 'H'
        b     _ram_fail

_ram_pass:
        li    r1, .Lstr_ram_ok
        bl    _early_puts
        mov   r13, r9
        ret

// On entry: r5 = bad readback, r6/r7 = step-code ASCII chars.
// (r6/r7 are clobbered by _dump_r5, so we emit them first.)
_ram_fail:
        li    r1, .Lstr_ram_fail
        bl    _early_puts
        mov   r2, r6;  bl _early_putc
        mov   r2, r7;  bl _early_putc
        li    r1, .Lstr_ram_got
        bl    _early_puts
        bl    _dump_r5             // r5 holds the bad value
        li    r1, .Lstr_crlf
        bl    _early_puts
        mov   r13, r9
        ret

        .size   _ram_check, . - _ram_check

// String literals.  Live alongside code in .text — the ROM has no
// real .text/.rodata distinction (both end up in the same 64 KB
// segment), and inlining them here avoids a section switch.
.Lstr_ram_header:  .asciz "\r\nRAM: "
.Lstr_ram_ok:      .asciz "OK\r\n"
.Lstr_ram_fail:    .asciz "FAIL "
.Lstr_ram_got:     .asciz " got "
.Lstr_crlf:        .asciz "\r\n"

// ────────────────────────────────────────────────────────────────
// _dump_r5 — print r5 as 8 hex chars (MSB first).  Returns to caller.
// Saves outer LR in r10 across nested BL calls to _early_putc.
// Clobbers r2/r3/r5/r6/r7/r10/r13.
// ────────────────────────────────────────────────────────────────
        .globl  _dump_r5
        .type   _dump_r5,@function
_dump_r5:
        mov   r10, r13             // save caller's return address
        li    r6, 8                // 8 nibbles to print, MSB first
_hex_loop:
        mov   r7, r5
        shr   r7, 28               // top nibble
        and   r7, 0xF
        cmp   r7, 10
        bge   _hex_alpha
        add   r7, '0'              // → '0'..'9'
        b     _hex_emit
_hex_alpha:
        add   r7, 'A' - 10         // → 'A'..'F'
_hex_emit:
        mov   r2, r7
        bl    _early_putc
        shl   r5, 4
        sub   r6, 1
        cmp   r6, 0
        bne   _hex_loop
        mov   r13, r10             // restore caller's return address
        ret
        .size   _dump_r5, . - _dump_r5

// ────────────────────────────────────────────────────────────────
// _early_puts — print NUL-terminated string at r1, byte by byte.
// Caller sets r1 = string address, r11 = LSR_THRE, r12 = UART_BASE.
// Saves caller LR in r10.  Does not nest with _dump_r5.
// Clobbers r1 (advances), r2, r3, r10, r13.
// ────────────────────────────────────────────────────────────────
        .globl  _early_puts
        .type   _early_puts,@function
_early_puts:
        mov   r10, r13
_puts_loop:
        ldb   r2, [r1]
        cmp   r2, 0
        beq   _puts_done
        bl    _early_putc
        add   r1, 1
        b     _puts_loop
_puts_done:
        mov   r13, r10
        ret
        .size   _early_puts, . - _early_puts

// ────────────────────────────────────────────────────────────────
// _early_putc — emit byte in r2[7:0] to UART, polling LSR.THRE.
// Caller sets r12 = UART_BASE, r11 = LSR_THRE mask.
// Clobbers r3.  Returns via r13 (bl/ret).
// ────────────────────────────────────────────────────────────────
        .globl  _early_putc
        .type   _early_putc,@function
_early_putc:
        ldw   r3, [r12 + UART_LSR]
        test  r3, r11
        beq   _early_putc          // THRE clear ⇒ wait
        stw   r2, [r12]
        ret
        .size   _early_putc, . - _early_putc
