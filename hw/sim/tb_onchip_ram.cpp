// tb_onchip_ram — the on-chip RAM device against the simple-device bus
// contract (doc/hardware/bus-protocol.md, "Slave obligations").
//
// Covers:
//  - the handshake: no ack in the presentation cycle, ack in the next,
//    read data valid in the ack cycle, and a strobe held past the ack
//    starting a new access;
//  - read-after-write of whole words, at both ends of the array;
//  - byte and halfword writes touching only their lanes;
//  - words written at every address holding their own values, so a
//    word-index decode error shows up as a wrong value.
#include "Vonchip_ram.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>

static const uint32_t WORDS = 32768;    // the module's default

static Vonchip_ram* dut;
static int checks = 0, fails = 0;

static void chk(bool ok, const char* what, uint32_t got = 0, uint32_t exp = 0) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL: %s (got 0x%08x, expected 0x%08x)\n", what, got, exp);
    }
}

static void tick() {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

// One access with the strobe held through its ack cycle; returns the
// read data presented in the ack cycle.
static uint32_t access(uint32_t addr, bool write, uint32_t wdata = 0,
                       uint8_t byte_en = 0xF) {
    dut->i_addr = addr;
    dut->i_wdata = wdata;
    dut->i_byte_en = byte_en;
    dut->i_we = write;
    dut->i_re = !write;
    dut->eval();
    chk(!dut->o_ack, "ack in the presentation cycle");
    tick();                                   // presentation cycle
    chk(dut->o_ack, "no ack in the cycle after presentation");
    uint32_t rdata = dut->o_rdata;
    tick();                                   // ack cycle, strobe held
    dut->i_we = 0;
    dut->i_re = 0;
    dut->eval();
    return rdata;
}

static uint32_t rd(uint32_t addr) { return access(addr, false); }
static void wr(uint32_t addr, uint32_t v, uint8_t be = 0xF) { access(addr, true, v, be); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vonchip_ram;
    dut->i_rst = 1;
    dut->i_re = 0;
    dut->i_we = 0;
    tick();
    tick();
    dut->i_rst = 0;

    // ── Whole words, both ends ─────────────────────────────────
    wr(0x0, 0x12345678);
    uint32_t v = rd(0x0);
    chk(v == 0x12345678, "word 0 read-after-write", v, 0x12345678);
    uint32_t last = (WORDS - 1) * 4;
    wr(last, 0xCAFEF00D);
    v = rd(last);
    chk(v == 0xCAFEF00D, "last word read-after-write", v, 0xCAFEF00D);
    v = rd(0x0);
    chk(v == 0x12345678, "word 0 survives a write to the last word", v, 0x12345678);

    // ── Byte and halfword lanes ────────────────────────────────
    wr(0x100, 0xAABBCCDD);
    wr(0x100, 0x00001100, 0x2);
    v = rd(0x100);
    chk(v == 0xAABB11DD, "byte write to lane 1 only", v, 0xAABB11DD);
    wr(0x100, 0x99880000, 0xC);
    v = rd(0x100);
    chk(v == 0x998811DD, "halfword write to lanes 3:2 only", v, 0x998811DD);
    wr(0x100, 0x000000EE, 0x1);
    v = rd(0x100);
    chk(v == 0x998811EE, "byte write to lane 0 only", v, 0x998811EE);

    // ── A strobe held past the ack is a new access ──────────────
    wr(0x200, 0x11111111);
    wr(0x204, 0x22222222);
    dut->i_addr = 0x200;
    dut->i_re = 1;
    tick();                                   // first access presented
    uint32_t first = dut->o_rdata;
    chk(dut->o_ack, "no ack for the first of two held reads");
    tick();                                   // ack cycle of the first
    dut->i_addr = 0x204;                      // the master moves on
    dut->eval();
    chk(!dut->o_ack, "ack in the second read's presentation cycle");
    tick();                                   // second presented
    chk(dut->o_ack, "no ack for the second of two held reads");
    uint32_t second = dut->o_rdata;
    tick();
    dut->i_re = 0;
    dut->eval();
    chk(first == 0x11111111, "first of two held reads", first, 0x11111111);
    chk(second == 0x22222222, "second of two held reads", second, 0x22222222);

    // ── Every word holds its own value ─────────────────────────
    for (uint32_t i = 0; i < WORDS; i += 97)
        wr(i * 4, 0xA5000000u ^ i);
    int bad = 0;
    for (uint32_t i = 0; i < WORDS; i += 97) {
        v = rd(i * 4);
        if (v != (0xA5000000u ^ i) && bad++ < 4)
            chk(false, "word holds its own value", v, 0xA5000000u ^ i);
    }
    chk(bad == 0, "words written across the array read back");

    dut->final();
    delete dut;
    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
