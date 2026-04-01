/*
 * boot_rom.c — Penumbra boot ROM
 */

#include "uart.h"
#include "libc.h"

typedef void (*trap_handler)(void);

#define TRAP_VECTORS ((volatile trap_handler *)0x00000000)

#define TRAP_BUS_FAULT 0
#define TRAP_EXT_IRQ   1
#define TRAP_TLB_MISS  2
#define TRAP_TLB_PROT  3
#define TRAP_PRIV_INST 4
#define TRAP_SYSCALL   5
#define TRAP_BREAK     6
#define TRAP_ILL_INST  7
#define TRAP_ALIGN_FLT 8

#define NUM_TRAPS      16

static void console_putc(char c) {
    uart_write(c);
}

static void console_puts(const char *s) {
    while (*s) {
        uart_write(*s++);
    }
}

/*
 * console_gets — read a line with editing into buffer (max chars incl. NUL).
 *
 * Supports:
 *   - Printable characters (0x20–0x7E): insert at cursor
 *   - Backspace (0x08) or DEL (0x7F): delete character before cursor
 *   - Ctrl-U (0x15): kill entire line
 *   - Enter (\r or \n): accept line
 *
 * Echoes characters as typed. Backspace sends "\b \b" (back, space, back)
 * to erase the character on the terminal.
 *
 * Returns the length of the entered string (not counting NUL).
 */
static int console_gets(char *buffer, int max) {
    int i = 0;

    for (;;) {
        int c = uart_read();

        if (c == '\r' || c == '\n') {
            console_puts("\r\n");
            break;
        }

        switch (c) {
            case 0x08:
            case 0x7F:
                if (i > 0) {
                    i--;
                    console_puts("\b \b");
                }
                break;
            case 0x15:
                for (; i>0; i--) {
                    console_puts("\b \b");
                }
                break;
            
            default:
                if (isprint(c) && i < (max - 1)) {
                    buffer[i++] = (char)c;
                    console_putc(c);
                }
        }
    }

    buffer[i] = '\0';
    return i;
}

/*
 * Formatted output helpers — no varargs needed.
 * console_put_dec / console_put_hex print numbers directly.
 * console_printf is not available (no G_VAARG support in backend yet).
 */
static void console_put_unsigned(unsigned int val, int base) {
    char buf[12];
    console_puts(utoa(val, buf, base));
}

static void console_put_dec(int val) {
    if (val < 0) {
        console_putc('-');
        val = -val;
    }
    console_put_unsigned((unsigned int)val, 10);
}

static void console_put_hex(unsigned int val) {
    console_puts("0x");
    console_put_unsigned(val, 16);
}

static const char *trap_name(int trapno) {
    switch (trapno) {
    case 0: return "BUS FAULT";
    case 1: return "IRQ";
    case 2: return "TLB MISS";
    case 3: return "TLB PROT";
    case 4: return "PRIV";
    case 5: return "SYSCALL";
    case 6: return "BREAK";
    case 7: return "ILLEGAL";
    case 8: return "ALIGN";
    default: return "UNKNOWN";
    }
}

/* Called from assembly trampolines in trap_entry.s.
 * R1=trapno, R2=EPC, R3=MMU FAULT_ADDR */
void unhandled_trap(int trapno, unsigned int epc, unsigned int fault_addr) {
    console_puts("\r\n*** TRAP #");
    console_put_dec(trapno);
    console_puts(": ");
    console_puts(trap_name(trapno));
    console_puts(" ***\r\n");
    console_puts("  EPC=");
    console_put_hex(epc);
    console_puts("  FAULT_ADDR=");
    console_put_hex(fault_addr);
    console_puts("\r\n");
    /* Trampoline executes BREAK after we return, halting the simulator. */
}

/* Assembly trampolines — defined in trap_entry.s */
extern void _trap_bus_fault(void);
extern void _trap_irq(void);
extern void _trap_tlb_miss(void);
extern void _trap_tlb_prot(void);
extern void _trap_priv(void);
extern void _trap_syscall(void);
extern void _trap_break(void);
extern void _trap_illegal(void);
extern void _trap_align(void);

static void setup_traps(void) {
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_fault;
    TRAP_VECTORS[TRAP_EXT_IRQ]   = _trap_irq;
    TRAP_VECTORS[TRAP_TLB_MISS]  = _trap_tlb_miss;
    TRAP_VECTORS[TRAP_TLB_PROT]  = _trap_tlb_prot;
    TRAP_VECTORS[TRAP_PRIV_INST] = _trap_priv;
    TRAP_VECTORS[TRAP_SYSCALL]   = _trap_syscall;
    TRAP_VECTORS[TRAP_BREAK]     = _trap_break;
    TRAP_VECTORS[TRAP_ILL_INST]  = _trap_illegal;
    TRAP_VECTORS[TRAP_ALIGN_FLT] = _trap_align;
}

extern void _trap_bus_ignore(void);
extern long _detect_page(long pagenum);

static long detect_ram() {
    void (*prev_vector)(void) = TRAP_VECTORS[TRAP_BUS_FAULT];
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;

    // There must be at least 2 pages mapped at 0x00000000 for this ROM to work
    // We must not touch those pages as they contain the ROM stack and other data
    long npages = 2;
    while (_detect_page(++npages))
        ;

    TRAP_VECTORS[TRAP_BUS_FAULT] = prev_vector;
    return npages;
}

int main(void) {
    char cmdbuffer[64];

    console_puts("Penumbra boot\r\n");
    console_puts("-------------\r\n");
    console_puts("\r\n");

    setup_traps();

    console_puts("Detecting base RAM... ");
    long npages = detect_ram();
    long ram_kb = npages * 4096 / 1024;
    console_put_dec(ram_kb);
    console_puts("kB found\r\n");
    console_puts("\r\n");

    /* Spin — placeholder for command loop */
    for (;;) {
        console_puts("> ");

        int len = console_gets(cmdbuffer, sizeof(cmdbuffer));
        if (len > 0) {
            /* Command parsing and handling goes here */
        }
    }
}
