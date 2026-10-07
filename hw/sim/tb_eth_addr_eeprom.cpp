// Verilator testbench for the station address reader
//
// The reader and an I2C master, with a model of a 24AA025E48 on the
// bus's two lines: a memory of 256 bytes behind an address pointer, as
// the part's datasheet describes its reads. The model answers whatever
// it is asked, so what the checks hold the reader to is what it asks:
//
//  * The read begins by itself behind a reset, once the master has
//    freed the bus.
//  * One transaction: the pointer set to where the part keeps its
//    address and nothing written, six bytes read, the first five
//    acknowledged and the last not, a STOP, and the bus let go.
//  * The address presented is those six bytes, the first in the low
//    byte, and is marked valid only once all six are in.
//  * A part that does not answer, one that refuses the word address, and
//    a bus error each end with the address not valid and nothing more
//    asked of the bus.
//
// The bus's timing is the master's own testbench's to check.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include "Veth_addr_eeprom_test.h"
#include "i2c_slave.h"

namespace {

Veth_addr_eeprom_test* dut;
int pass = 0, fail = 0;
long now = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL [cycle %ld: ", now); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 100) {
        printf("eth_addr_eeprom: gave up after %d failures\n", fail);
        exit(1);
    }
}

// ── The EEPROM ───────────────────────────────────────────────
const uint8_t PART_ADDR = 0x57;         // as the harness straps it
const int     EUI_AT    = 0xFA;         // where the part keeps its address
const int     ADDR_ACKS = 5;            // bytes of it a read acknowledges: all but the last

I2cSlave part(PART_ADDR);
uint8_t  memory[256];
uint8_t  pointer = 0;                   // the part's address pointer
int      pointer_sets = 0;              // times a word address was written
int      bytes_written = 0;             // bytes written behind a word address
int      starts = 0, stops = 0;        // the read's own, not the master's as it frees the bus
int      read_from = -1;                // where the pointer stood as a read began

// At every START and STOP: a byte written since the last is the word
// address, any more are data, and a read sends from the pointer on.
void part_boundary() {
    if (!part.written.empty()) {
        pointer = part.written[0];
        pointer_sets++;
        bytes_written += (int)part.written.size() - 1;
        part.written.clear();
    }
    part.to_send.clear();
    for (int i = 0; i < 32; i++)
        part.to_send.push_back(memory[(pointer + i) & 0xff]);
}

void fill_memory(const uint8_t eui[6]) {
    // Every other byte is its own address turned over, so a read from
    // the wrong place shows.
    for (int i = 0; i < 256; i++)
        memory[i] = (uint8_t)~i;
    for (int i = 0; i < 6; i++)
        memory[EUI_AT + i] = eui[i];
}

// ── The bus ──────────────────────────────────────────────────
bool hold_sda = false;                  // the testbench's own grip on the line
bool bus_freed = false;                 // the master's STOP behind a reset has been seen
bool scl_was = true, sda_was = true;
long bus_moved = -1;                    // the last cycle either line changed in

void tick() {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
    part.tick();
    bool scl = dut->o_scl;
    bool sda = dut->o_sda && !part.drive_low && !hold_sda;
    dut->i_scl = scl; dut->i_sda = sda;

    if (scl != scl_was || sda != sda_was)
        bus_moved = now;
    // Behind a reset the master frees the bus before it takes a command,
    // and ends that with a STOP of its own.
    if (scl && scl_was && sda != sda_was) {
        if (!bus_freed)
            bus_freed = sda;
        else if (sda)
            stops++;
        else
            starts++;
    }
    // Where the pointer stands as the part first begins to send.
    bool sending_before = (part.st == I2cSlave::SEND);
    part.observe(scl, sda);
    if (!sending_before && part.st == I2cSlave::SEND && read_from < 0)
        read_from = pointer;
    scl_was = scl; sda_was = sda;
    now++;
}

void run(long cycles) {
    for (long i = 0; i < cycles; i++)
        tick();
}

// The board's reset lasts 10 ms at least: 40000 cycles of this clock.
const long BOARD_RESET = 40000;

// A whole read is nine bytes of nine bits at forty cycles a bit, and the
// conditions around them: well inside this.
const long READ_CYCLES = 9 * 9 * 40 + 2000;

void reset() {
    dut->i_rst = 1;
    run(BOARD_RESET - 100);
    // A fresh part, while the reset still holds: one cut off as it sent
    // lets the data line go here, and the line settles before the master
    // looks at it.
    part = I2cSlave(PART_ADDR);
    part.on_boundary = part_boundary;
    run(100);
    dut->i_rst = 0;
    pointer = 0;
    pointer_sets = bytes_written = starts = stops = 0;
    bus_freed = false;
    read_from = -1;
}

uint64_t as_presented(const uint8_t eui[6]) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++)
        v |= (uint64_t)eui[i] << (8 * i);
    return v;
}

// The read, from the reset's end to the STOP that ends its transaction.
void read_to_the_end(const char* what) {
    long limit = now + READ_CYCLES;
    while (stops < 1 && now < limit) {
        check(!dut->o_valid, "%s: the address is marked valid before the read is over", what);
        tick();
    }
    check(stops == 1, "%s: the read's transaction did not end in %ld cycles", what, READ_CYCLES);
    run(200);
    check(dut->i_scl && dut->i_sda, "%s: the bus is not let go (SCL %d, SDA %d)", what,
          dut->i_scl, dut->i_sda);
}

void expect_address(const char* what, const uint8_t eui[6]) {
    check(dut->o_valid, "%s: the address is not marked valid", what);
    check(dut->o_addr == as_presented(eui),
          "%s: the address presented is %012llx, the part holds %012llx", what,
          (unsigned long long)dut->o_addr, (unsigned long long)as_presented(eui));
    check(read_from >= 0, "%s: the part was never read", what);
    check(pointer_sets == 1 && (read_from < 0 || read_from == EUI_AT),
          "%s: the pointer was set %d times and the read began at %02x, not once and at %02x", what,
          pointer_sets, read_from & 0xff, EUI_AT);
    check(bytes_written == 0, "%s: %d bytes were written to the part", what, bytes_written);
    check(part.acks == ADDR_ACKS && part.nacks == 1,
          "%s: of the bytes read, %d were acknowledged and %d not: five and one are wanted", what,
          part.acks, part.nacks);
    check(starts == 2 && stops == 1,
          "%s: %d STARTs and %d STOPs, not two and one", what, starts, stops);
    check(part.st == I2cSlave::IDLE, "%s: the part is left part way through a transfer", what);
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Veth_addr_eeprom_test;
    dut->i_scl = 1; dut->i_sda = 1;

    const uint8_t eui_a[6] = {0x00, 0x04, 0xA3, 0x12, 0x34, 0x56};
    const uint8_t eui_b[6] = {0xFC, 0x0F, 0xE7, 0x80, 0x01, 0xFF};

    // The read.
    fill_memory(eui_a);
    reset();
    check(!dut->o_valid, "an address is marked valid behind a reset");
    read_to_the_end("the read");
    expect_address("the read", eui_a);

    // The address holds, and the bus stays quiet.
    long after = now;
    run(20000);
    check(dut->o_valid && dut->o_addr == as_presented(eui_a), "the address did not hold");
    check(bus_moved < after, "the bus moved at cycle %ld after the read", bus_moved);

    // Another part's address, behind a reset.
    fill_memory(eui_b);
    reset();
    check(!dut->o_valid, "the first part's address is still marked valid behind a reset");
    read_to_the_end("a second part");
    expect_address("a second part", eui_b);

    // No part at the address.
    fill_memory(eui_a);
    reset();
    part.address = 0x50;
    read_to_the_end("no part at the address");
    check(!dut->o_valid, "no part at the address: an address is marked valid");
    check(starts == 1 && stops == 1,
          "no part at the address: %d STARTs and %d STOPs, not one of each", starts, stops);

    // A part that refuses the word address.
    reset();
    part.ack_data = false;
    read_to_the_end("the word address refused");
    check(!dut->o_valid, "the word address refused: an address is marked valid");

    // A bus error: the data line held low, so the master's START finds
    // the bus taken. The reader gives up, and asks nothing more once the
    // line is let go.
    reset();
    hold_sda = true;
    run(2 * READ_CYCLES);
    check(!dut->o_valid, "the data line held: an address is marked valid");
    hold_sda = false;
    run(200);
    {
        long quiet_from = now;
        run(2 * READ_CYCLES);
        check(bus_moved < quiet_from,
              "the data line let go: the bus moved at cycle %ld, behind a read given up",
              bus_moved);
        check(!dut->o_valid, "the data line let go: an address is marked valid");
    }

    // A reset part way through the read, and the read again.
    reset();
    run(READ_CYCLES / 3);
    check(starts >= 1 && stops < 1 && !dut->o_valid,
          "a third of the way through: %d STARTs, %d STOPs and valid %d", starts, stops,
          dut->o_valid);
    reset();
    read_to_the_end("a read behind one cut short");
    expect_address("a read behind one cut short", eui_a);

    printf("eth_addr_eeprom: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}
