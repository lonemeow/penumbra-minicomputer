/*
 * boot_rom.c — Penumbra boot ROM
 */

#include "uart.h"
#include "libc.h"
#include "penumbra.h"
#include <stdarg.h>

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

/* Formatted output helpers. */
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

static void console_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_puts(buf);
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
 * SPR/sysreg reads done here via inline asm (penumbra.h) so we can
 * easily add diagnostics without touching assembly. */
void unhandled_trap(int trapno) {
    uint32_t epc        = penumbra_read_spr(SPR_EPC);
    uint32_t esr        = penumbra_read_spr(SPR_ESR);
    uint32_t fault_addr = penumbra_read_sysreg(SYSDEV_MMU, MMU_FAULT_ADDR);
    uint32_t fault_stat = penumbra_read_sysreg(SYSDEV_MMU, MMU_FAULT_STATUS);

    console_printf("\r\nTRAP %d (%s) pc=0x%x", trapno, trap_name(trapno), epc);
    console_printf(" sr=0x%x fa=0x%x fs=0x%x\r\n", esr, fault_addr, fault_stat);

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

/*
 * Probe the last word of a page to test if it is backed by RAM.
 * Write a pattern, read it back — if it matches, the page exists.
 * If not (bus fault), _trap_bus_ignore advances EPC and we return 0.
 */
static int detect_page(long pagenum) {
    volatile unsigned int *addr =
        (volatile unsigned int *)(((pagenum + 1) << 12) - 4);
    *addr = 0x12345678;
    return *addr == 0x12345678;
}

static long detect_ram(void) {
    void (*prev_vector)(void) = TRAP_VECTORS[TRAP_BUS_FAULT];
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;

    /* Pages 0-1 are reserved (vector table, trap stack, boot data) */
    long npages = 2;
    while (detect_page(npages))
        npages++;

    TRAP_VECTORS[TRAP_BUS_FAULT] = prev_vector;
    return npages;
}

/*
 * Autoconfig — enumerate devices on the bus via config chain.
 *
 * Returns the number of devices found. Device info is printed
 * to the console for diagnostics. With no devices on the chain,
 * the first config read faults immediately and we return 0.
 */
static const char *class_name(uint32_t cls) {
    switch (cls) {
    case ACFG_CLASS_MEMORY: return "Memory";
    case ACFG_CLASS_UART:   return "UART";
    case ACFG_CLASS_SPI:    return "SPI";
    default:                return "Unknown";
    }
}

/*
 * Try to read a word from addr. If the read bus-faults (no device
 * responds), _trap_bus_ignore skips the LDW and the register keeps
 * the sentinel value 0xFFFFFFFF.
 *
 * Uses inline asm to guarantee a single LDW instruction — at -O0
 * the compiler might insert extra loads/stores around a volatile
 * read that break the skip-one-instruction pattern.
 */
static uint32_t bus_probe_read(uint32_t addr) {
    uint32_t val;
    /* Two instructions: set sentinel, then try the load.
     * If LDW bus-faults, _trap_bus_ignore skips it and val keeps 0xFFFFFFFF.
     * Early-clobber (&) ensures val and addr get different registers. */
    asm volatile(
        "lli %0, #0xFFFF\n\t"
        "lui %0, #0xFFFF\n\t"
        "ldw %0, [%1]"
        : "=&r"(val) : "r"(addr) : "memory"
    );
    return val;
}

static int autoconfig(void) {
    int ndevs = 0;
    uint32_t next_io_addr = 0xFF001000;

    /* Assert bus reset to clear any stale device configs */
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_RST);

    /* Brief delay for async bus devices */
    for (volatile int i = 0; i < 100; i++) {}

    /* Deassert reset, enable config mode */
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, BUSCTL_CFG_EN);

    /* Install bus-fault-ignore handler for probing */
    void (*prev_vector)(void) = TRAP_VECTORS[TRAP_BUS_FAULT];
    TRAP_VECTORS[TRAP_BUS_FAULT] = _trap_bus_ignore;

    for (;;) {
        /* Probe: read CFG_CLASS, sentinel 0xFFFFFFFF means bus fault */
        uint32_t cls = bus_probe_read(AUTOCONFIG_BASE + 0x00);
        if (cls == 0xFFFFFFFF)
            break;

        uint32_t size = bus_probe_read(AUTOCONFIG_BASE + 0x04);
        uint32_t id   = bus_probe_read(AUTOCONFIG_BASE + 0x08);

        /* Read device name (4 words → 16 bytes, word-aligned) */
        uint32_t name_words[5];  /* 5th word for null terminator space */
        name_words[0] = bus_probe_read(AUTOCONFIG_BASE + 0x0C);
        name_words[1] = bus_probe_read(AUTOCONFIG_BASE + 0x10);
        name_words[2] = bus_probe_read(AUTOCONFIG_BASE + 0x14);
        name_words[3] = bus_probe_read(AUTOCONFIG_BASE + 0x18);
        name_words[4] = 0;
        char *name = (char *)name_words;

        /* Allocate base address and configure the device */
        uint32_t base;
        if (cls == ACFG_CLASS_MEMORY) {
            base = 0x01000000;  /* TODO: track actual system RAM end */
        } else {
            uint32_t mask = size - 1;
            base = (next_io_addr + mask) & ~mask;
            next_io_addr = base + size;
        }
        *(volatile uint32_t *)(AUTOCONFIG_BASE + 0x1C) = base;

        console_printf("  %s %s @ 0x%x (%d bytes)\r\n",
                        name, class_name(cls), base, (int)size);

        ndevs++;
    }

    /* Restore and disable config mode */
    TRAP_VECTORS[TRAP_BUS_FAULT] = prev_vector;
    penumbra_write_sysreg(SYSDEV_BUS, BUS_CTL, 0);

    return ndevs;
}

int main(void) {
    char cmdbuffer[64];

    console_puts("Penumbra/1 boot\r\n\r\n");

    setup_traps();

    console_puts("Detecting base RAM... ");
    long npages = detect_ram();
    long ram_kb = npages * 4096 / 1024;
    console_printf("%dkB found\r\n", (int)ram_kb);

    console_puts("Probing bus devices...\r\n");
    int ndevs = autoconfig();
    console_printf("%d device(s) found\r\n\r\n", ndevs);

    /* Monitor command loop */
    for (;;) {
        console_puts("> ");

        int len = console_gets(cmdbuffer, sizeof(cmdbuffer));
        if (len > 0) {
            if (strcmp(cmdbuffer, "b") == 0 ||
                strcmp(cmdbuffer, "break") == 0) {
                asm volatile("break");
            }
        }
    }
}
