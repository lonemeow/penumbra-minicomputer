// Verilator testbench for penumbra2_mem_stage.
//
// Drives the EX/MEM input, the pipeline handshake, and a behavioral model of
// the BRAM data memory across clock edges, checking the MEM/WB register, the
// back-pressure, and the data-side work against the MEM-stage contract in
// doc/internals/penumbra2/pipeline-stages.md:
//   - reset clears valid
//   - a pass-through op (ALU / divmul / WRSPR) latches in 1 cycle, no stall
//   - a faulting slot advances with its fault tag intact (WB suppresses)
//   - a load takes 2 cycles when the data side is ready: cycle 1 launches the
//     lookup + stalls EX + bubbles MEM/WB, cycle 2 presents o_dmem_re and
//     delivers the extracted/extended sub-word
//   - byte / half / word loads extract the right lane, sign- or zero-extend
//   - a store launches the lookup too (the cache's tag compare needs it),
//     presents its lane-replicated write level from the data-ready cycle, and
//     the write commits with the correct byte-enable at the completion cycle
//   - a busy data side (i_dmem_busy: line fill / downstream round trip) holds
//     the slot — stall held, bubbles into MEM/WB, request level held — and the
//     access completes on the busy-drop cycle (drop-equals-valid read data)
//   - a misaligned load/store faults (VEC_ALIGN) in 1 cycle, no memory access
//   - downstream stall coincident with a fresh access defers the launch
//     (no premature read) and leaves the held MEM/WB slot intact
//   - i_bubble coincident with an access's launch suppresses it (no lookup,
//     no write) and flushes the slot; it can never land on a *launched*
//     access — the stage asserts that, and the harness never drives it
//
// The data memory mirrors the L1 front side (cache_bram_vipt) with a
// configurable completion delay: the launch (o_dmem_en) samples the address
// and arms mem_latency busy cycles; the read is served from the held address
// so it is valid on the busy-drop cycle; the held write applies exactly once,
// at the completion cycle's edge. mem_latency = 0 degenerates to
// unified_mem's hit-always registered read.
//
// The MMU port-B verdict is modeled as permanently clean (i_mmu_fault low —
// the stage consumes only the fault leg; the paddr goes to the cache's tag
// compare, outside this stage). Translation faults are exercised at machine
// level, not here.
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

// ── Behavioral data memory (mirrors the L1 front side) ──
// 256 words; word-addressed; the launch (o_dmem_en) samples the address and
// arms mem_latency busy cycles; reads serve from the held address so the data
// is valid on the busy-drop cycle; the level-held write applies exactly once,
// at the completion (busy-low request) cycle's edge; read-before-write.
static uint32_t dmem[256];
static uint32_t dmem_rdata_reg;
static int      mem_latency;          // busy cycles per access (0 = hit timing)
static int      busy_count;           // countdown for the in-flight access

static void mem_init() {
    for (int i = 0; i < 256; i++) dmem[i] = 0;
    dmem_rdata_reg = 0;
    mem_latency = 0;
    busy_count = 0;
}

// One clock with the memory model sampled at the rising edge: capture the
// combinational dmem drive while the clock is low (those outputs feed the
// upcoming posedge), apply the read/write at the posedge, then present the
// registered read and the busy level for the next cycle.
static void tick(Vpenumbra2_mem_stage* dut) {
    dut->i_clk = 0; dut->eval();
    bool en = dut->o_dmem_en, we = dut->o_dmem_we, re = dut->o_dmem_re;
    bool busy = dut->i_dmem_busy;         // as presented during this cycle
    uint32_t widx = (dut->o_dmem_addr >> 2) & 0xFF;
    uint32_t wd = dut->o_dmem_wdata;
    uint8_t  be = dut->o_dmem_byte_en;
    dut->i_clk = 1; dut->eval();          // posedge: DUT registers update

    dut->i_mmu_fault = 0;                 // port-B verdict: always clean here

    if (en) busy_count = mem_latency;     // launch arms the completion delay
    else if (busy_count > 0) busy_count--;

    // Read serve: continuous from the held address (idempotent — single
    // master, no concurrent writer), so it is valid whenever busy drops.
    if (en || re) dmem_rdata_reg = dmem[widx];
    if (we && !busy) {                    // completion edge: apply the write
        uint32_t w = dmem[widx];
        for (int b = 0; b < 4; b++)
            if (be & (1u << b)) {
                w &= ~(0xFFu << (8 * b));
                w |= (wd & (0xFFu << (8 * b)));
            }
        dmem[widx] = w;
    }
    dut->i_dmem_rdata = dmem_rdata_reg;
    dut->i_dmem_busy  = (busy_count > 0);
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
    check("ldw_c1_dmem_en", dut->o_dmem_en, 1);  // ...and launches the lookup
    check("ldw_c1_no_re",   dut->o_dmem_re, 0);  // request starts at data-ready
    tick(dut); dut->eval();
    check("ldw_c1_no_commit", dut->o_valid, 0);  // MEM/WB bubbled during access
    check("ldw_c2_re",        dut->o_dmem_re, 1); // read request presented level
    check("ldw_c2_release",   dut->o_stall, 0);  // cycle 2 releases (busy low)
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
    check("stw_c1_en",   dut->o_dmem_en, 1);      // a store launches the lookup too
    check("stw_c1_nowe", dut->o_dmem_we, 0);      // no write request on the launch cycle
    tick(dut); dut->eval();
    check("stw_c2_we",   dut->o_dmem_we, 1);      // write presented from data-ready on
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

    // ── Busy data side holds a load until the busy-drop completion ──
    // A 3-cycle completion delay (a short line fill). The stall and the
    // MEM/WB bubble hold through every busy cycle, the read request stays
    // presented level, and the value lands on the busy-drop cycle —
    // drop-equals-valid, no extra cycle after it.
    dmem[0x30 >> 2] = 0x0BADF00D;
    mem_latency = 3;
    clear(dut);
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_mem_size = SZ_WORD;
    dut->i_gpr_we = 1; dut->i_result = 0x30; dut->i_phys_dst = 8; dut->i_valid = 1;
    dut->eval();
    check("ldbusy_c1_stall", dut->o_stall, 1);      // launch cycle
    tick(dut); dut->eval();
    for (int w = 0; w < 3; w++) {                    // busy-wait cycles
        check("ldbusy_wait_busy",  dut->i_dmem_busy, 1);
        check("ldbusy_wait_stall", dut->o_stall, 1);
        check("ldbusy_wait_re",    dut->o_dmem_re, 1);   // request held level
        check("ldbusy_wait_hold",  dut->o_valid, 0);     // still bubbling
        tick(dut); dut->eval();
    }
    check("ldbusy_drop_stall", dut->o_stall, 0);    // completes on the drop cycle
    tick(dut); dut->eval();
    check("ldbusy_value", dut->o_wb_value, 0x0BADF00D);
    check("ldbusy_valid", dut->o_valid, 1);
    mem_latency = 0;

    // ── Busy data side holds a store; the write commits exactly once ──
    dmem[0x50 >> 2] = 0x01020304;
    mem_latency = 2;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_WORD;
    dut->i_result = 0x50; dut->i_store_data = 0x55AA55AA; dut->i_valid = 1;
    dut->eval();
    tick(dut); dut->eval();                          // launch
    for (int w = 0; w < 2; w++) {                    // busy-wait cycles
        check("stbusy_wait_we",   dut->o_dmem_we, 1);     // write held level
        check("stbusy_wait_mem",  dmem[0x50 >> 2], 0x01020304);  // not committed yet
        check("stbusy_wait_stall", dut->o_stall, 1);
        tick(dut); dut->eval();
    }
    check("stbusy_drop_we", dut->o_dmem_we, 1);      // still presented at completion
    tick(dut); dut->eval();                          // completion edge commits it
    check("stbusy_mem",   dmem[0x50 >> 2], 0x55AA55AA);
    check("stbusy_valid", dut->o_valid, 1);
    mem_latency = 0;

    // ── Downstream stall while a fresh access tries to launch ────
    // The mirror, at the unit level, of the divmul-aux-hold hazard: WB holds a
    // prior slot in MEM/WB (i_stall_in) just as a memory op arrives *fresh*
    // (acc_phase 0). The launch must be DEFERRED — no premature read, and the
    // held slot left intact — until WB releases, then the access launches and
    // completes cleanly. (Before mem_first was gated by ~i_stall_in, the launch
    // fired into the held slot and clobbered it.)
    dmem[0x38 >> 2] = 0xD15EA5ED;
    clear(dut);
    // Land a prior ALU result in MEM/WB so we can prove the stall holds it.
    dut->i_op_class = OPC_ALU; dut->i_gpr_we = 1;
    dut->i_result = 0xA5A5A5A5; dut->i_phys_dst = 5; dut->i_valid = 1;
    dut->eval();
    tick(dut);                               // ALU advances into MEM/WB
    // A fresh load now arrives while WB back-pressures (still holding the ALU).
    dut->i_op_class = OPC_LOAD; dut->i_mem_op = MEM_LOAD; dut->i_mem_size = SZ_WORD;
    dut->i_gpr_we = 1; dut->i_result = 0x38; dut->i_phys_dst = 9; dut->i_valid = 1;
    dut->i_stall_in = 1;                      // WB cannot accept (e.g. divmul aux hold)
    dut->eval();

    // While i_stall_in is high the launch is deferred: EX is back-pressured,
    // no read is driven, and the prior ALU slot in MEM/WB is held intact.
    check("deferlaunch_o_stall",    dut->o_stall, 1);
    check("deferlaunch_o_dmem_en",  dut->o_dmem_en, 0);
    check("deferlaunch_o_valid",    dut->o_valid, 1);
    check("deferlaunch_o_wb_value", dut->o_wb_value, 0xA5A5A5A5);

    dut->i_stall_in = 0;                      // WB accepts; the load may launch now
    tick(dut); dut->eval();                   // cycle 1: launch (read driven)
    tick(dut); dut->eval();                   // cycle 2: data-ready, advances
    check("deferlaunch_value", dut->o_wb_value, 0xD15EA5ED);  // load completed
    check("deferlaunch_valid", dut->o_valid, 1);

    // ── i_bubble at a store's launch suppresses the access entirely ──
    // The flush can only ever coincide with a launch (WB's fault commit and
    // this slot's MEM entry are the same cycle — asserted in the stage), so
    // suppression is the whole story: no lookup, no write, slot flushed.
    dmem[0x4C >> 2] = 0xCAFED00D;
    clear(dut);
    dut->i_op_class = OPC_STORE; dut->i_mem_op = MEM_STORE; dut->i_mem_size = SZ_WORD;
    dut->i_result = 0x4C; dut->i_store_data = 0xFFFFFFFF; dut->i_valid = 1;
    dut->i_bubble = 1;
    dut->eval();
    check("bubble_store_no_en", dut->o_dmem_en, 0);  // lookup never launches
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
