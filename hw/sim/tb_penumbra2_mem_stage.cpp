// Verilator testbench for penumbra2_mem_stage.
//
// Drives the EX/MEM input, the pipeline handshake, and a behavioral model of
// the BRAM data memory across clock edges, checking the MEM/WB register, the
// back-pressure, and the data-side work against the MEM-stage contract in
// doc/internals/penumbra2/pipeline-stages.md:
//   - reset clears valid
//   - a pass-through op (ALU / divmul / WRSPR) latches in 1 cycle, no stall
//   - a faulting slot advances with its fault tag intact (WB suppresses)
//   - a load takes 2 cycles: cycle 1 stalls EX + bubbles MEM/WB, cycle 2
//     delivers the extracted/extended sub-word and advances
//   - byte / half / word loads extract the right lane, sign- or zero-extend
//   - a store takes 2 cycles and commits its lane-replicated write with the
//     correct byte-enable exactly on the advancing cycle
//   - a misaligned load/store faults (VEC_ALIGN) in 1 cycle, no memory access
//   - downstream stall on a load's data-ready cycle holds the result
//   - i_bubble cancels an in-flight store (no write) and flushes the slot
//
// The data memory mirrors bram_mem.sv: the address is sampled at the clock
// edge and the word appears the next cycle; o_dmem_en is the read clock-
// enable; writes are byte-enabled with no read/write-through.
//
// The deferred-path guard (no RDSYS may reach the stage) is an
// `always_comb assert`, so a failed `$error` aborts the sim (exit 1). Run
// `./Vpenumbra2_mem_stage +guard` to drive an RDSYS and watch it abort; the
// default run stays clean so the module-test exit code reflects only check().

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_mem_stage.h"
#include "verilated.h"

// op_class / mem_op / mem_size (penumbra2_pkg + penumbra_pkg)
enum { OPC_ALU = 0, OPC_LOAD = 1, OPC_STORE = 2, OPC_WRSPR = 7, OPC_RDSYS = 8 };
enum { MEM_NONE = 0, MEM_LOAD = 1, MEM_STORE = 2 };
enum { SZ_BYTE = 0, SZ_HALF = 1, SZ_WORD = 2 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

// ── Behavioral BRAM data memory (mirrors bram_mem.sv) ────────────
// 256 words; word-addressed; registered read (1-cycle latency) gated by
// o_dmem_en; byte-enabled write; read-before-write (no write-through).
static uint32_t dmem[256];
static uint32_t dmem_rdata_reg;

static void mem_init() {
    for (int i = 0; i < 256; i++) dmem[i] = 0;
    dmem_rdata_reg = 0;
}

// One clock with the memory model sampled at the rising edge: capture the
// combinational dmem drive while the clock is low (those outputs feed the
// upcoming posedge), apply the read/write at the posedge, then present the
// registered read so it is visible during the next cycle.
static void tick(Vpenumbra2_mem_stage* dut) {
    dut->i_clk = 0; dut->eval();
    bool en = dut->o_dmem_en, we = dut->o_dmem_we;
    uint32_t widx = (dut->o_dmem_addr >> 2) & 0xFF;
    uint32_t wd = dut->o_dmem_wdata;
    uint8_t  be = dut->o_dmem_byte_en;

    dut->i_clk = 1; dut->eval();          // posedge: DUT registers update

    if (en) dmem_rdata_reg = dmem[widx];  // read old value first
    if (we) {                             // then apply the byte-enabled write
        uint32_t w = dmem[widx];
        for (int b = 0; b < 4; b++)
            if (be & (1u << b)) {
                w &= ~(0xFFu << (8 * b));
                w |= (wd & (0xFFu << (8 * b)));
            }
        dmem[widx] = w;
    }
    dut->i_dmem_rdata = dmem_rdata_reg;
    dut->eval();
}

// Reset the per-cycle inputs to a quiet baseline: a valid-but-bubble ALU
// slot, no memory op, no writes, no stall/flush.
static void clear(Vpenumbra2_mem_stage* dut) {
    dut->i_op_class = OPC_ALU; dut->i_mem_op = MEM_NONE;
    dut->i_mem_size = SZ_WORD; dut->i_sign_ext = 0;
    dut->i_gpr_we = 0; dut->i_spr_we = 0; dut->i_flag_we = 0; dut->i_spr_sel = 0;
    dut->i_result = 0; dut->i_result_aux = 0; dut->i_store_data = 0;
    dut->i_flag_value = 0;
    dut->i_phys_dst = 0; dut->i_phys_dst_aux = 0; dut->i_phys_dst_aux_en = 0;
    dut->i_pc = 0; dut->i_valid = 0; dut->i_fault_pending = 0; dut->i_fault_vec = 0;
    dut->i_stall_in = 0; dut->i_bubble = 0;
}

// Drive one aligned load and run the 2-cycle access; return o_wb_value after
// it advances. ea must be aligned for the size.
static uint32_t do_load(Vpenumbra2_mem_stage* dut, uint32_t ea, int size, int sext) {
    clear(dut);
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD;
    dut->i_mem_size = size; dut->i_sign_ext = sext;
    dut->i_gpr_we = 1; dut->i_result = ea; dut->i_phys_dst = 4; dut->i_valid = 1;
    dut->eval();
    // cycle 1: launch — stall EX, no advance
    tick(dut); dut->eval();
    // cycle 2: data ready — advances (inputs held stable by the harness)
    tick(dut); dut->eval();
    return dut->o_wb_value;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    bool run_guard = Verilated::commandArgsPlusMatch("guard")[0] != '\0';
    Vpenumbra2_mem_stage* dut = new Vpenumbra2_mem_stage;
    mem_init();

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_valid_low", dut->o_valid, 0);

    // ── ALU op: pass-through in 1 cycle, no stall ────────────────
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1; dut->i_flag_we = 1;
    dut->i_result = 0xCAFEBABE; dut->i_flag_value = 0x5;
    dut->i_phys_dst = 7; dut->i_pc = 0xFFFF0100; dut->i_valid = 1;
    dut->eval();
    check("alu_no_stall",   dut->o_stall, 0);
    tick(dut); dut->eval();
    check("alu_valid",      dut->o_valid, 1);
    check("alu_gpr_we",     dut->o_gpr_we, 1);
    check("alu_wb_value",   dut->o_wb_value, 0xCAFEBABE);
    check("alu_flag_value", dut->o_flag_value, 0x5);
    check("alu_phys_dst",   dut->o_phys_dst, 7);
    check("alu_pc",         dut->o_pc, 0xFFFF0100);

    // ── divmul two-half payload passes through ───────────────────
    clear(dut);
    dut->i_gpr_we = 1;
    dut->i_result = 0x11112222; dut->i_result_aux = 0x33334444;
    dut->i_phys_dst = 1; dut->i_phys_dst_aux = 2; dut->i_phys_dst_aux_en = 1;
    dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("dm_wb_lo",      dut->o_wb_value, 0x11112222);
    check("dm_wb_hi",      dut->o_wb_value_aux, 0x33334444);
    check("dm_dst_aux_en", dut->o_phys_dst_aux_en, 1);

    // ── WRSPR value reaches WB on the shared datum ───────────────
    clear(dut);
    dut->i_op_class = OPC_WRSPR; dut->i_spr_we = 1; dut->i_spr_sel = 3;
    dut->i_result = 0xDEADBEEF; dut->i_valid = 1;
    dut->eval(); tick(dut); dut->eval();
    check("wrspr_spr_we",   dut->o_spr_we, 1);
    check("wrspr_wb_value", dut->o_wb_value, 0xDEADBEEF);
    check("wrspr_op_class", dut->o_op_class, OPC_WRSPR);

    // ════════════════════════════════════════════════════════════
    // Loads — preload memory, then read sub-words back.
    // ════════════════════════════════════════════════════════════
    dmem[0x10 >> 2] = 0x8899AABB;   // word at 0x10
    dmem[0x20 >> 2] = 0x7F80817E;   // word at 0x20 (byte/half extract patterns)

    // ── Word load: 2-cycle access, stall then data ───────────────
    clear(dut);
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_mem_size = SZ_WORD;
    dut->i_gpr_we = 1; dut->i_result = 0x10; dut->i_phys_dst = 4; dut->i_valid = 1;
    dut->eval();
    check("ldw_c1_stall",  dut->o_stall, 1);     // cycle 1 stalls EX
    check("ldw_c1_dmem_en", dut->o_dmem_en, 1);  // ...and launches the read
    tick(dut); dut->eval();
    check("ldw_c1_no_commit", dut->o_valid, 0);  // MEM/WB bubbled during access
    check("ldw_c2_release",   dut->o_stall, 0);  // cycle 2 releases
    tick(dut); dut->eval();
    check("ldw_valid",     dut->o_valid, 1);
    check("ldw_value",     dut->o_wb_value, 0x8899AABB);
    check("ldw_gpr_we",    dut->o_gpr_we, 1);
    check("ldw_phys_dst",  dut->o_phys_dst, 4);

    // ── Sub-word loads from 0x20 = 0x7F80817E (LE: b0=7E b1=81 b2=80 b3=7F)
    check("ldbu_off0", do_load(dut, 0x20, SZ_BYTE, 0), 0x0000007E);
    check("ldbu_off1", do_load(dut, 0x21, SZ_BYTE, 0), 0x00000081);
    check("ldbs_off1", do_load(dut, 0x21, SZ_BYTE, 1), 0xFFFFFF81);  // sign-extend
    check("ldbs_off0", do_load(dut, 0x20, SZ_BYTE, 1), 0x0000007E);  // positive
    check("ldhu_lo",   do_load(dut, 0x20, SZ_HALF, 0), 0x0000817E);
    check("ldhu_hi",   do_load(dut, 0x22, SZ_HALF, 0), 0x00007F80);
    check("ldhs_lo",   do_load(dut, 0x20, SZ_HALF, 1), 0xFFFF817E);  // sign-extend

    // ════════════════════════════════════════════════════════════
    // Stores — write, then verify byte-enable landed only the right lanes.
    // ════════════════════════════════════════════════════════════
    // Word store
    dmem[0x40 >> 2] = 0x00000000;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_WORD;
    dut->i_result = 0x40; dut->i_store_data = 0x12345678; dut->i_valid = 1;
    dut->eval();
    check("stw_c1_be",   dut->o_dmem_byte_en, 0xF);
    check("stw_c1_nowe", dut->o_dmem_we, 0);      // no write on the launch cycle
    tick(dut); dut->eval();
    check("stw_c2_we",   dut->o_dmem_we, 1);      // write commits on the advancing cycle
    tick(dut); dut->eval();
    check("stw_mem", dmem[0x40 >> 2], 0x12345678);

    // Byte store to offset 2 — only lane 2 changes, replicated data
    dmem[0x44 >> 2] = 0xAABBCCDD;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_BYTE;
    dut->i_result = 0x46; dut->i_store_data = 0x00000099; dut->i_valid = 1;
    dut->eval();
    check("stb_be", dut->o_dmem_byte_en, 0x4);    // lane 2 (EA[1:0]=10)
    tick(dut); dut->eval(); tick(dut); dut->eval();
    check("stb_mem", dmem[0x44 >> 2], 0xAA99CCDD);

    // Halfword store to high half — lanes 2,3 change
    dmem[0x48 >> 2] = 0x11223344;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_HALF;
    dut->i_result = 0x4A; dut->i_store_data = 0x0000BEEF; dut->i_valid = 1;
    dut->eval();
    check("sth_be", dut->o_dmem_byte_en, 0xC);    // lanes 3:2 (EA[1]=1)
    tick(dut); dut->eval(); tick(dut); dut->eval();
    check("sth_mem", dmem[0x48 >> 2], 0xBEEF3344);

    // ── Misaligned access: 1-cycle alignment fault, no memory access ──
    clear(dut);
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_mem_size = SZ_WORD;
    dut->i_gpr_we = 1; dut->i_result = 0x12;  // word @ +2 → misaligned
    dut->i_phys_dst = 6; dut->i_valid = 1;
    dut->eval();
    check("misalign_no_stall", dut->o_stall, 0);   // no access → single cycle
    check("misalign_no_en",    dut->o_dmem_en, 0);
    tick(dut); dut->eval();
    check("misalign_valid", dut->o_valid, 1);
    check("misalign_fault", dut->o_fault_pending, 1);
    check("misalign_vec",   dut->o_fault_vec, 8);  // VEC_ALIGN

    // ── Faulting slot (upstream fault) passes through, no access ──
    clear(dut);
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1; dut->i_phys_dst = 5;
    dut->i_valid = 1; dut->i_fault_pending = 1; dut->i_fault_vec = 10;
    dut->eval(); tick(dut); dut->eval();
    check("fault_valid", dut->o_valid, 1);
    check("fault_vec",   dut->o_fault_vec, 10);

    // ── Downstream stall on a load's data-ready cycle holds it ───
    dmem[0x30 >> 2] = 0x0BADF00D;
    clear(dut);
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_mem_size = SZ_WORD;
    dut->i_gpr_we = 1; dut->i_result = 0x30; dut->i_phys_dst = 8; dut->i_valid = 1;
    dut->eval();
    tick(dut); dut->eval();                 // cycle 1: launch
    dut->i_stall_in = 1;                     // WB back-pressures the data-ready cycle
    dut->eval();
    check("ldstall_hold_stall", dut->o_stall, 1);
    tick(dut); dut->eval();
    check("ldstall_held", dut->o_valid, 0);  // not advanced yet (still bubble)
    dut->i_stall_in = 0;                      // release
    dut->eval();
    tick(dut); dut->eval();
    check("ldstall_value", dut->o_wb_value, 0x0BADF00D);
    check("ldstall_valid", dut->o_valid, 1);

    // ── i_bubble cancels an in-flight store (no write) and flushes ──
    dmem[0x4C >> 2] = 0xCAFED00D;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_WORD;
    dut->i_result = 0x4C; dut->i_store_data = 0xFFFFFFFF; dut->i_valid = 1;
    dut->i_bubble = 1;
    dut->eval();
    check("bubble_store_no_we", dut->o_dmem_we, 0);
    tick(dut); dut->eval();
    check("bubble_flush_valid", dut->o_valid, 0);
    check("bubble_store_mem",   dmem[0x4C >> 2], 0xCAFED00D);  // memory untouched

    // ── Deferred-path guard demo (opt-in, aborts the sim) ────────
    // RDSYS's sysreg sideband is not wired; driving one trips the guard's
    // $error and aborts with a non-zero exit. Loads/stores no longer trip it.
    if (run_guard) {
        printf("  [+guard] driving an RDSYS — expect the guard to abort the sim:\n");
        clear(dut); dut->i_op_class = OPC_RDSYS; dut->i_valid = 1;
        dut->eval();
    }

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
