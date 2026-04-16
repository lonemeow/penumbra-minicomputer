// Verilator testbench for Penumbra SPI Master v2
//
// Tests the real spi.sv with hardware shift register, FIFOs,
// transfer engine, and IRQ logic. Uses a small FIFO (8 entries)
// and fast divider (SLOW_DIV=1, FAST_DIV=0) for quick simulation.
//
// Test groups:
//   1. CAP register / reset state
//   2. Single-byte polled transfer (legacy path, FIFO_EN=0)
//   3. FIFO push/pop and flush
//   4. Burst transfer engine
//   5. IRQ assertion and clearing
//   6. Watermark (level-based) IRQ behavior

#include <cstdio>
#include <cstdint>
#include <cstring>
#include "Vspi.h"

static int errors = 0, tests = 0;

// DUT with small FIFO for fast tests.
// Verilator instantiation: spi #(.FIFO_DEPTH(8), .SLOW_DIV(1), .FAST_DIV(0))
// (Overridden via -GFIFO_DEPTH=8 -GSLOW_DIV=1 -GFAST_DIV=0 on command line)

static Vspi* dut;

// ── Helpers ────────────────────────────────────────────────

static void tick() {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void ticks(int n) {
    for (int i = 0; i < n; i++) tick();
}

static void reset() {
    dut->i_rst = 1;
    dut->i_we = 0;
    dut->i_re = 0;
    dut->i_addr = 0;
    dut->i_wdata = 0;
    dut->i_miso = 1;  // Idle high (SD convention)
    tick(); tick();
    dut->i_rst = 0;
}

// Register offsets (byte addresses)
enum {
    REG_CAP        = 0x00,
    REG_STATUS     = 0x04,
    REG_CONTROL    = 0x08,
    REG_DATA       = 0x0C,
    REG_XFER_COUNT = 0x10,
    REG_IRQ_STATUS = 0x14,
    REG_IRQ_ENABLE = 0x18
};

// CONTROL bits
enum {
    CTL_CS0     = 0x01,
    CTL_CS1     = 0x02,
    CTL_CPOL    = 0x10,
    CTL_CPHA    = 0x20,
    CTL_FAST    = 0x40,
    CTL_FIFO_EN = 0x80,
    CTL_FLUSH_TX = (1 << 14),
    CTL_FLUSH_RX = (1 << 15)
};

// STATUS bits
enum {
    ST_SPI_BUSY = 0x01,
    ST_SPI_DONE = 0x02
};

// IRQ bits
enum {
    IRQ_XFER_DONE = 0x01,
    IRQ_RX_THRESH = 0x02,
    IRQ_TX_THRESH = 0x04
};

static void reg_write(uint32_t offset, uint32_t data) {
    dut->i_addr = offset;
    dut->i_wdata = data;
    dut->i_we = 1;
    tick();
    dut->i_we = 0;
}

static uint32_t reg_read(uint32_t offset) {
    dut->i_addr = offset;
    dut->i_re = 1;
    tick();  // Rising edge: o_rdata latched from rdata_next
    uint32_t val = dut->o_rdata;  // Valid now (i_re was 1)
    dut->i_re = 0;
    tick();  // Cleanup: o_rdata clears, access_pending resets
    return val;
}

// Wait for SPI_BUSY to clear (max cycles to avoid hang)
static bool wait_not_busy(int max_cycles = 500) {
    for (int i = 0; i < max_cycles; i++) {
        uint32_t st = reg_read(REG_STATUS);
        if (!(st & ST_SPI_BUSY))
            return true;
    }
    return false;
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

#define CHECKV(name, got, expected) do { \
    tests++; \
    if ((got) != (expected)) { \
        errors++; \
        printf("  FAIL: %s: got 0x%x, expected 0x%x\n", name, \
               (unsigned)(got), (unsigned)(expected)); \
    } \
} while (0)

// ── Test: reset state and CAP register ─────────────────────

static void test_reset_and_cap() {
    printf("test_reset_and_cap\n");
    reset();

    uint32_t cap = reg_read(REG_CAP);
    CHECKV("CAP version", cap & 0xFF, 1);
    CHECKV("CAP FIFO depth", (cap >> 8) & 0xFFFF, 8);

    uint32_t st = reg_read(REG_STATUS);
    CHECK("STATUS not busy after reset", !(st & ST_SPI_BUSY));
    CHECK("STATUS not done after reset", !(st & ST_SPI_DONE));

    uint32_t ctl = reg_read(REG_CONTROL);
    CHECKV("CONTROL reset default", ctl, 0x03);  // CS0=1, CS1=1

    CHECK("CS0 deasserted (high)", dut->o_cs0 == 1);
    CHECK("CS1 deasserted (high)", dut->o_cs1 == 1);
    CHECK("IRQ not asserted", dut->o_irq == 0);

    uint32_t irqen = reg_read(REG_IRQ_ENABLE);
    CHECKV("IRQ_ENABLE reset 0", irqen, 0);
}

// ── Test: single-byte polled transfer (FIFO_EN=0) ──────────

static void test_single_byte_polled() {
    printf("test_single_byte_polled\n");
    reset();

    // Assert CS0
    reg_write(REG_CONTROL, CTL_CS1);  // CS0=0 (asserted), CS1=1
    CHECK("CS0 asserted (low)", dut->o_cs0 == 0);

    // Drive MISO with a fixed pattern: alternate 1/0 per bit
    // For simplicity, just drive MISO=0 throughout.
    // The SPI controller samples MISO, so rx_data will be 0x00.
    dut->i_miso = 0;

    // Write DATA to start transfer (send 0xA5)
    reg_write(REG_DATA, 0xA5);

    // Should be busy
    uint32_t st = reg_read(REG_STATUS);
    CHECK("SPI_BUSY after DATA write", st & ST_SPI_BUSY);

    // Wait for completion
    bool done = wait_not_busy();
    CHECK("Transfer completes", done);

    st = reg_read(REG_STATUS);
    CHECK("SPI_DONE set after transfer", st & ST_SPI_DONE);

    // Read received data (MISO was 0 throughout)
    uint32_t rx = reg_read(REG_DATA);
    CHECKV("RX data (MISO=0)", rx & 0xFF, 0x00);

    // Now transfer with MISO=1 to verify we get 0xFF
    dut->i_miso = 1;
    reg_write(REG_DATA, 0x00);
    done = wait_not_busy();
    CHECK("Second transfer completes", done);
    rx = reg_read(REG_DATA);
    CHECKV("RX data (MISO=1)", rx & 0xFF, 0xFF);
}

// ── Test: FIFO push/pop and flush ───────────────────────────

static void test_fifo_push_pop() {
    printf("test_fifo_push_pop\n");
    reset();

    // Enable FIFO mode
    reg_write(REG_CONTROL, CTL_CS1 | CTL_FIFO_EN);

    // Push 4 bytes into TX FIFO
    for (int i = 0; i < 4; i++)
        reg_write(REG_DATA, 0x10 + i);

    uint32_t st = reg_read(REG_STATUS);
    uint32_t tx_level = (st >> 4) & 0xFFF;
    CHECKV("TX level after 4 pushes", tx_level, 4);
    CHECK("TX not empty", !(st & (1u << 28)));
    CHECK("TX not full", !(st & (1u << 29)));

    // Push 4 more — should be full (FIFO_DEPTH=8)
    for (int i = 0; i < 4; i++)
        reg_write(REG_DATA, 0x20 + i);

    st = reg_read(REG_STATUS);
    tx_level = (st >> 4) & 0xFFF;
    CHECKV("TX level after 8 pushes", tx_level, 8);
    CHECK("TX full", st & (1u << 29));

    // Flush TX
    reg_write(REG_CONTROL, CTL_CS1 | CTL_FIFO_EN | CTL_FLUSH_TX);
    st = reg_read(REG_STATUS);
    tx_level = (st >> 4) & 0xFFF;
    CHECKV("TX level after flush", tx_level, 0);
    CHECK("TX empty after flush", st & (1u << 28));
}

// ── Test: burst transfer engine ─────────────────────────────

static void test_burst_transfer() {
    printf("test_burst_transfer\n");
    reset();

    // Enable FIFO + FAST clock
    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);

    // Push 5 bytes to TX
    for (int i = 0; i < 5; i++)
        reg_write(REG_DATA, 0xA0 + i);

    // Drive MISO=0 (all RX will be 0x00)
    dut->i_miso = 0;

    // Start engine: transfer 5 bytes
    reg_write(REG_XFER_COUNT, 5 | (1 << 16));

    // Wait for engine to finish
    bool done = false;
    for (int i = 0; i < 2000; i++) {
        tick();
        uint32_t irqst = reg_read(REG_IRQ_STATUS);
        if (irqst & IRQ_XFER_DONE) {
            done = true;
            break;
        }
    }
    CHECK("Engine completes transfer", done);

    // Check RX FIFO has 5 bytes
    uint32_t st = reg_read(REG_STATUS);
    uint32_t rx_level = (st >> 16) & 0xFFF;
    CHECKV("RX level after burst", rx_level, 5);

    // Pop and verify all 0x00 (MISO was 0)
    for (int i = 0; i < 5; i++) {
        uint32_t rx = reg_read(REG_DATA);
        CHECKV("RX FIFO byte", rx & 0xFF, 0x00);
    }

    // RX should be empty now
    st = reg_read(REG_STATUS);
    CHECK("RX empty after drain", st & (1u << 30));
}

// ── Test: XFER_DONE IRQ assert and W1C ──────────────────────

static void test_irq_xfer_done() {
    printf("test_irq_xfer_done\n");
    reset();

    // Enable FIFO + FAST, enable XFER_DONE IRQ
    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);
    reg_write(REG_IRQ_ENABLE, IRQ_XFER_DONE);

    dut->i_miso = 1;

    // Push one TX byte, then start 1-byte transfer
    reg_write(REG_DATA, 0xFF);
    reg_write(REG_XFER_COUNT, 1 | (1 << 16));

    // Wait for IRQ
    bool irq_fired = false;
    for (int i = 0; i < 500; i++) {
        tick();
        if (dut->o_irq) {
            irq_fired = true;
            break;
        }
    }
    CHECK("o_irq asserts on XFER_DONE", irq_fired);

    // Read IRQ_STATUS
    uint32_t irqst = reg_read(REG_IRQ_STATUS);
    CHECK("XFER_DONE flag set", irqst & IRQ_XFER_DONE);

    // W1C: clear XFER_DONE
    reg_write(REG_IRQ_STATUS, IRQ_XFER_DONE);
    ticks(2);
    CHECK("o_irq deasserts after W1C", dut->o_irq == 0);

    irqst = reg_read(REG_IRQ_STATUS);
    CHECK("XFER_DONE cleared by W1C", !(irqst & IRQ_XFER_DONE));
}

// ── Test: zero-count transfer fires XFER_DONE immediately ───

static void test_zero_count() {
    printf("test_zero_count\n");
    reset();

    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);
    reg_write(REG_IRQ_ENABLE, IRQ_XFER_DONE);

    // Start with count=0
    reg_write(REG_XFER_COUNT, 0 | (1 << 16));
    ticks(4);

    uint32_t irqst = reg_read(REG_IRQ_STATUS);
    CHECK("XFER_DONE on zero count", irqst & IRQ_XFER_DONE);
}

// ── Test: watermark IRQ (live level, not latched) ───────────

static void test_watermark_irq() {
    printf("test_watermark_irq\n");
    reset();

    // Enable FIFO, enable TX_THRESH IRQ
    // TX_THRESH fires when tx_level <= DEPTH/2 = 4
    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);
    reg_write(REG_IRQ_ENABLE, IRQ_TX_THRESH);

    // TX FIFO is empty (level=0 <= 4), so TX_THRESH should be live
    ticks(2);
    CHECK("TX_THRESH asserted when empty", dut->o_irq == 1);

    uint32_t irqst = reg_read(REG_IRQ_STATUS);
    CHECK("TX_THRESH in IRQ_STATUS", irqst & IRQ_TX_THRESH);

    // Fill TX FIFO above threshold (push 5 bytes → level=5 > 4)
    for (int i = 0; i < 5; i++)
        reg_write(REG_DATA, 0x42);

    ticks(2);
    CHECK("TX_THRESH deasserts when above threshold", dut->o_irq == 0);

    irqst = reg_read(REG_IRQ_STATUS);
    CHECK("TX_THRESH cleared in IRQ_STATUS", !(irqst & IRQ_TX_THRESH));
}

// ── Test: CONTROL register FAST/SLOW bit ────────────────────

static void test_fast_slow_clock() {
    printf("test_fast_slow_clock\n");
    reset();

    // FIFO_EN=0, slow clock. Transfer and count cycles.
    dut->i_miso = 1;

    // Slow transfer (SLOW_DIV=1, so period = 2*(1+1) = 4 cycles per bit,
    // 8 bits = 32 cycles for the shift + some overhead)
    reg_write(REG_DATA, 0x55);
    int slow_cycles = 0;
    while (slow_cycles < 1000) {
        tick();
        slow_cycles++;
        uint32_t st = reg_read(REG_STATUS);
        if (!(st & ST_SPI_BUSY))
            break;
    }

    // Fast transfer (FAST_DIV=0, period = 2*(0+1) = 2 cycles per bit,
    // 8 bits = 16 cycles)
    reg_write(REG_CONTROL, CTL_CS0 | CTL_CS1 | CTL_FAST);
    reg_write(REG_DATA, 0xAA);
    int fast_cycles = 0;
    while (fast_cycles < 1000) {
        tick();
        fast_cycles++;
        uint32_t st = reg_read(REG_STATUS);
        if (!(st & ST_SPI_BUSY))
            break;
    }

    CHECK("Fast transfer quicker than slow", fast_cycles < slow_cycles);
    // Sanity check: slow should take roughly 2x fast for div=1 vs div=0
    CHECK("Slow transfer not instant", slow_cycles > 10);
    CHECK("Fast transfer not instant", fast_cycles > 5);
}

// ── Test: MOSI output matches TX data ───────────────────────

static void test_mosi_output() {
    printf("test_mosi_output\n");
    reset();

    // Transmit 0x80 (MSB first → MOSI should be 1 first, then 0s)
    dut->i_miso = 0;
    reg_write(REG_DATA, 0x80);

    // After the DATA write, o_mosi should reflect shift_out[7]
    // which is the MSB of 0x80 = 1
    tick();
    CHECK("MOSI is MSB of 0x80", dut->o_mosi == 1);

    // Let transfer finish
    wait_not_busy();
}

// ── Test: RX full stall — engine pauses until drained ───────

static void test_rx_full_stall() {
    printf("test_rx_full_stall\n");
    reset();

    // Enable FIFO + FAST, enable XFER_DONE IRQ
    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);
    reg_write(REG_IRQ_ENABLE, IRQ_XFER_DONE);

    dut->i_miso = 1;   // All RX bytes will be 0xFF

    // Fill TX FIFO with 8 bytes (full) and start 12-byte transfer.
    // After 8 bytes shifted, RX FIFO fills up → engine stalls.
    for (int i = 0; i < 8; i++)
        reg_write(REG_DATA, 0x50 + i);

    reg_write(REG_XFER_COUNT, 12 | (1 << 16));

    // Run until engine stalls (RX full, no XFER_DONE yet).
    // Give enough cycles for 8 bytes to shift through.
    for (int i = 0; i < 2000; i++)
        tick();

    // Engine should NOT have completed — it's stalled on RX full
    uint32_t irqst = reg_read(REG_IRQ_STATUS);
    CHECK("XFER_DONE not yet set (stalled)", !(irqst & IRQ_XFER_DONE));

    uint32_t st = reg_read(REG_STATUS);
    CHECK("RX FIFO full (stall point)", st & (1u << 31));

    // Drain 4 bytes from RX FIFO to make room
    for (int i = 0; i < 4; i++)
        reg_read(REG_DATA);

    // Also need to refill TX FIFO — engine stalls on TX empty too.
    // We started with 8 TX bytes for a 12-byte transfer, so we need 4 more.
    for (int i = 0; i < 4; i++)
        reg_write(REG_DATA, 0x60 + i);

    // Now let engine resume and finish
    bool done = false;
    for (int i = 0; i < 3000; i++) {
        tick();
        // Keep draining RX if it fills up again
        st = reg_read(REG_STATUS);
        if (st & (1u << 31)) {
            for (int j = 0; j < 4; j++)
                reg_read(REG_DATA);
        }
        irqst = reg_read(REG_IRQ_STATUS);
        if (irqst & IRQ_XFER_DONE) {
            done = true;
            break;
        }
    }
    CHECK("Engine completes after RX drain", done);

    // Drain remaining RX bytes
    st = reg_read(REG_STATUS);
    uint32_t rx_level = (st >> 16) & 0xFFF;
    for (uint32_t i = 0; i < rx_level; i++)
        reg_read(REG_DATA);

    st = reg_read(REG_STATUS);
    CHECK("RX empty after full drain", st & (1u << 30));
}

// ── Test: TX empty stall — engine pauses until refilled ─────

static void test_tx_empty_stall() {
    printf("test_tx_empty_stall\n");
    reset();

    // Enable FIFO + FAST, enable XFER_DONE IRQ
    reg_write(REG_CONTROL, CTL_FIFO_EN | CTL_FAST);
    reg_write(REG_IRQ_ENABLE, IRQ_XFER_DONE);

    dut->i_miso = 0;

    // Push only 2 bytes to TX, but start a 5-byte transfer.
    // Engine processes 2 bytes, then stalls waiting for TX data.
    reg_write(REG_DATA, 0xAA);
    reg_write(REG_DATA, 0xBB);

    reg_write(REG_XFER_COUNT, 5 | (1 << 16));

    // Let the 2 bytes shift through
    for (int i = 0; i < 500; i++)
        tick();

    // Engine should be stalled — not done yet
    uint32_t irqst = reg_read(REG_IRQ_STATUS);
    CHECK("XFER_DONE not yet set (TX stall)", !(irqst & IRQ_XFER_DONE));

    uint32_t st = reg_read(REG_STATUS);
    uint32_t tx_level = (st >> 4) & 0xFFF;
    CHECKV("TX empty during stall", tx_level, 0);

    // RX should have 2 bytes so far (the 2 that completed)
    uint32_t rx_level = (st >> 16) & 0xFFF;
    CHECKV("RX has 2 bytes mid-transfer", rx_level, 2);

    // Refill TX with remaining 3 bytes
    reg_write(REG_DATA, 0xCC);
    reg_write(REG_DATA, 0xDD);
    reg_write(REG_DATA, 0xEE);

    // Let engine finish
    bool done = false;
    for (int i = 0; i < 2000; i++) {
        tick();
        irqst = reg_read(REG_IRQ_STATUS);
        if (irqst & IRQ_XFER_DONE) {
            done = true;
            break;
        }
    }
    CHECK("Engine completes after TX refill", done);

    // RX should now have all 5 bytes
    st = reg_read(REG_STATUS);
    rx_level = (st >> 16) & 0xFFF;
    CHECKV("RX has all 5 bytes", rx_level, 5);

    // TX should be empty
    tx_level = (st >> 4) & 0xFFF;
    CHECKV("TX empty after completion", tx_level, 0);
}

// ── Main ───────────────────────────────────────────────────

int main() {
    dut = new Vspi;

    test_reset_and_cap();
    test_single_byte_polled();
    test_fifo_push_pop();
    test_burst_transfer();
    test_irq_xfer_done();
    test_zero_count();
    test_watermark_irq();
    test_fast_slow_clock();
    test_mosi_output();
    test_rx_full_stall();
    test_tx_empty_stall();

    printf("\nspi: %d/%d passed\n", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
