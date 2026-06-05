// Verilator testbench for unified_mem.
//
// Checks the unified dual-port memory stand-in's contract:
//   - port B byte-enabled write, registered read (1-cycle latency)
//   - the two ports share one array: a store on port B is visible on port A
//     (the unified property the exception vector-fetch relies on)
//   - the RAM/ROM region split (addr[31]) keeps same-low-bits addresses in
//     disjoint storage
//   - read clock-enable holds the output under a stall
//   - ROM (region 1) is read-only (write dropped) — observed via +guard, which
//     trips the sim-only assertion and aborts.
//
// No INIT_FILE here (ROM init is exercised by the core-level fetch tests);
// this focuses on the port/region behaviour.

#include <cstdio>
#include <cstdint>
#include "Vunified_mem.h"
#include "verilated.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vunified_mem* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void clear(Vunified_mem* dut) {
    dut->i_a_addr = 0; dut->i_a_en = 0;
    dut->i_b_addr = 0; dut->i_b_wdata = 0; dut->i_b_byte_en = 0;
    dut->i_b_we = 0; dut->i_b_en = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool run_guard = Verilated::commandArgsPlusMatch("guard")[0] != '\0';
    Vunified_mem* dut = new Vunified_mem;
    clear(dut); dut->eval();

    // ── Port B word write, then read it back (registered, 1-cycle) ──
    clear(dut);
    dut->i_b_addr = 0x100; dut->i_b_wdata = 0x12345678; dut->i_b_byte_en = 0xF; dut->i_b_we = 1;
    dut->eval(); tick(dut);
    clear(dut);
    dut->i_b_addr = 0x100; dut->i_b_en = 1;
    dut->eval(); tick(dut); dut->eval();
    check("b_word_rw", dut->o_b_rdata, 0x12345678);

    // ── Byte-enable: write only lane 2, leave the rest ──────────
    clear(dut);
    dut->i_b_addr = 0x100; dut->i_b_wdata = 0x00AB0000; dut->i_b_byte_en = 0x4; dut->i_b_we = 1;
    dut->eval(); tick(dut);
    clear(dut);
    dut->i_b_addr = 0x100; dut->i_b_en = 1;
    dut->eval(); tick(dut); dut->eval();
    check("b_byte_en", dut->o_b_rdata, 0x12AB5678);

    // ── Unified: a port-B store is visible on port A (same address) ──
    clear(dut);
    dut->i_b_addr = 0x200; dut->i_b_wdata = 0xCAFEF00D; dut->i_b_byte_en = 0xF; dut->i_b_we = 1;
    dut->eval(); tick(dut);
    clear(dut);
    dut->i_a_addr = 0x200; dut->i_a_en = 1;          // read the SAME address via the fetch port
    dut->eval(); tick(dut); dut->eval();
    check("unified_b_to_a", dut->o_a_rdata, 0xCAFEF00D);

    // ── Region split: RAM 0x40 and ROM 0xFFFF0040 are disjoint ──
    clear(dut);
    dut->i_b_addr = 0x40; dut->i_b_wdata = 0xAAAAAAAA; dut->i_b_byte_en = 0xF; dut->i_b_we = 1;
    dut->eval(); tick(dut);
    clear(dut);
    dut->i_a_addr = 0xFFFF0040; dut->i_a_en = 1;     // same low bits, ROM region
    dut->eval(); tick(dut); dut->eval();
    check("region_split_rom_zero", dut->o_a_rdata, 0x00000000);   // ROM untouched
    clear(dut);
    dut->i_a_addr = 0x40; dut->i_a_en = 1;            // RAM region holds the write
    dut->eval(); tick(dut); dut->eval();
    check("region_split_ram_set", dut->o_a_rdata, 0xAAAAAAAA);

    // ── Read clock-enable holds the output under a stall ────────
    clear(dut);
    dut->i_a_addr = 0x200; dut->i_a_en = 1;           // latch 0xCAFEF00D
    dut->eval(); tick(dut); dut->eval();
    check("ce_loaded", dut->o_a_rdata, 0xCAFEF00D);
    dut->i_a_addr = 0x40; dut->i_a_en = 0;            // en low: new address ignored, output frozen
    dut->eval(); tick(dut); dut->eval();
    check("ce_held", dut->o_a_rdata, 0xCAFEF00D);

    // ── ROM write-protect demo (opt-in, aborts the sim) ─────────
    if (run_guard) {
        printf("  [+guard] writing the ROM region — expect the assertion to abort:\n");
        clear(dut);
        dut->i_b_addr = 0xFFFF0000; dut->i_b_wdata = 0xDEAD; dut->i_b_byte_en = 0xF; dut->i_b_we = 1;
        dut->eval(); tick(dut);
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
