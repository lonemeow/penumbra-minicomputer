// Verilator testbench for the Penumbra Status Register
//
// Tests:
//   - Reset state (supervisor mode, interrupts disabled, flags clear)
//   - Flag latch from ALU via flag_w_en
//   - flag_w_en=0 does not modify flags
//   - Bulk load via sr_load (RTI/SETSR path)
//   - sr_load ignores reserved bits
//   - Exception entry: snapshot then S=1, I=0
//   - Exception entry preserves flags, only changes S and I
//   - EI sets I=1 and arms ei_shadow
//   - DI sets I=0
//   - ei_shadow auto-clears on i_ei_shadow_clr
//   - Priority: except_entry > sr_load > flag_w_en
//   - o_sr_read packing matches bit layout
//   - Full RTI round-trip: save SR via exception, restore via sr_load

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vstatus_reg.h"

static int errors = 0;
static int tests  = 0;

// SR bit positions (must match penumbra_pkg)
static const int SR_N = 0;
static const int SR_Z = 1;
static const int SR_C = 2;
static const int SR_V = 3;
static const int SR_I = 30;
static const int SR_S = 31;

static void tick(Vstatus_reg* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

static void check1(const char* name, int got, int expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got %d, expected %d\n", name, got, expected);
        errors++;
    }
}

// Clear all control inputs to safe defaults
static void clear_inputs(Vstatus_reg* dut) {
    dut->i_alu_flag_n   = 0;
    dut->i_alu_flag_z   = 0;
    dut->i_alu_flag_c   = 0;
    dut->i_alu_flag_v   = 0;
    dut->i_flag_w_en    = 0;
    dut->i_sr_load      = 0;
    dut->i_wdata        = 0;
    dut->i_except_entry = 0;
    dut->i_ei_set       = 0;
    dut->i_di_set       = 0;
    dut->i_ei_shadow_clr = 0;
}

static void reset(Vstatus_reg* dut) {
    clear_inputs(dut);
    dut->i_rst = 1;
    tick(dut);
    dut->i_rst = 0;
}

int main(int argc, char** argv) {
    Vstatus_reg* dut = new Vstatus_reg;

    // ── Reset state ────────────────────────────────────────────
    reset(dut);
    check1("reset_flag_n",    dut->o_flag_n,    0);
    check1("reset_flag_z",    dut->o_flag_z,    0);
    check1("reset_flag_c",    dut->o_flag_c,    0);
    check1("reset_flag_v",    dut->o_flag_v,    0);
    check1("reset_sr_s",      dut->o_sr_s,      1);  // supervisor
    check1("reset_sr_i",      dut->o_sr_i,      0);  // interrupts disabled
    check1("reset_ei_shadow", dut->o_ei_shadow, 0);
    // SR read: S=1 → bit 31 set, everything else 0
    check("reset_sr_read", dut->o_sr_read, (1u << SR_S));
    check("reset_shadow_sr", dut->o_shadow_sr, 0x00000000);

    // ── Flag latch from ALU ────────────────────────────────────
    clear_inputs(dut);
    dut->i_alu_flag_n = 1;
    dut->i_alu_flag_z = 0;
    dut->i_alu_flag_c = 1;
    dut->i_alu_flag_v = 1;
    dut->i_flag_w_en  = 1;
    tick(dut);
    clear_inputs(dut);
    check1("flag_latch_n", dut->o_flag_n, 1);
    check1("flag_latch_z", dut->o_flag_z, 0);
    check1("flag_latch_c", dut->o_flag_c, 1);
    check1("flag_latch_v", dut->o_flag_v, 1);
    // S and I unchanged
    check1("flag_latch_s_unchanged", dut->o_sr_s, 1);
    check1("flag_latch_i_unchanged", dut->o_sr_i, 0);

    // ── flag_w_en=0 does not modify flags ──────────────────────
    dut->i_alu_flag_n = 0;
    dut->i_alu_flag_z = 1;
    dut->i_alu_flag_c = 0;
    dut->i_alu_flag_v = 0;
    dut->i_flag_w_en  = 0;  // disabled
    tick(dut);
    clear_inputs(dut);
    check1("no_latch_n", dut->o_flag_n, 1);  // still old value
    check1("no_latch_z", dut->o_flag_z, 0);
    check1("no_latch_c", dut->o_flag_c, 1);
    check1("no_latch_v", dut->o_flag_v, 1);

    // ── o_sr_read packing ──────────────────────────────────────
    // Current state: N=1, Z=0, C=1, V=1, S=1, I=0
    uint32_t expected_sr = (1u << SR_N) | (1u << SR_C) | (1u << SR_V) | (1u << SR_S);
    check("sr_read_packing", dut->o_sr_read, expected_sr);

    // ── Bulk load via sr_load ──────────────────────────────────
    // Load: N=0, Z=1, C=0, V=0, S=0, I=1 (user mode, interrupts on)
    uint32_t load_val = (1u << SR_Z) | (1u << SR_I);
    dut->i_sr_load = 1;
    dut->i_wdata   = load_val;
    tick(dut);
    clear_inputs(dut);
    check1("sr_load_n", dut->o_flag_n, 0);
    check1("sr_load_z", dut->o_flag_z, 1);
    check1("sr_load_c", dut->o_flag_c, 0);
    check1("sr_load_v", dut->o_flag_v, 0);
    check1("sr_load_s", dut->o_sr_s,   0);
    check1("sr_load_i", dut->o_sr_i,   1);
    check("sr_load_readback", dut->o_sr_read, load_val);

    // ── sr_load ignores reserved bits ──────────────────────────
    // Set garbage in reserved bits [29:4], only defined bits should matter
    uint32_t garbage_load = 0xFFFFFFFF;  // all bits set
    dut->i_sr_load = 1;
    dut->i_wdata   = garbage_load;
    tick(dut);
    clear_inputs(dut);
    // All defined bits should be 1
    check1("sr_load_reserved_n", dut->o_flag_n, 1);
    check1("sr_load_reserved_z", dut->o_flag_z, 1);
    check1("sr_load_reserved_c", dut->o_flag_c, 1);
    check1("sr_load_reserved_v", dut->o_flag_v, 1);
    check1("sr_load_reserved_s", dut->o_sr_s,   1);
    check1("sr_load_reserved_i", dut->o_sr_i,   1);
    // But sr_read should have only the defined bits (reserved = 0)
    uint32_t all_defined = (1u << SR_N) | (1u << SR_Z) | (1u << SR_C) | (1u << SR_V)
                         | (1u << SR_I) | (1u << SR_S);
    check("sr_load_reserved_read", dut->o_sr_read, all_defined);

    // ── Exception entry ────────────────────────────────────────
    // Set up a known state first: user mode, interrupts on, some flags
    reset(dut);
    // Bulk load to user mode with I=1 and N=1, C=1
    uint32_t pre_except = (1u << SR_N) | (1u << SR_C) | (1u << SR_I);
    dut->i_sr_load = 1;
    dut->i_wdata   = pre_except;
    tick(dut);
    clear_inputs(dut);
    // Verify pre-exception state
    check1("pre_except_s", dut->o_sr_s, 0);
    check1("pre_except_i", dut->o_sr_i, 1);

    // Fire exception entry
    dut->i_except_entry = 1;
    tick(dut);
    clear_inputs(dut);
    // shadow_sr should capture the pre-exception state
    check("except_shadow", dut->o_shadow_sr, pre_except);
    // SR should now be: S=1, I=0, flags unchanged
    check1("except_s_set",     dut->o_sr_s,   1);
    check1("except_i_cleared", dut->o_sr_i,   0);
    check1("except_n_kept",    dut->o_flag_n, 1);
    check1("except_c_kept",    dut->o_flag_c, 1);
    check1("except_z_kept",    dut->o_flag_z, 0);
    check1("except_v_kept",    dut->o_flag_v, 0);

    // ── EI / DI ────────────────────────────────────────────────
    reset(dut);
    // EI: sets I=1 and arms ei_shadow
    dut->i_ei_set = 1;
    tick(dut);
    clear_inputs(dut);
    check1("ei_sr_i",      dut->o_sr_i,      1);
    check1("ei_shadow_set", dut->o_ei_shadow, 1);

    // ei_shadow auto-clears on clr signal
    dut->i_ei_shadow_clr = 1;
    tick(dut);
    clear_inputs(dut);
    check1("ei_shadow_cleared", dut->o_ei_shadow, 0);
    check1("ei_i_still_set",    dut->o_sr_i,      1);  // I stays on

    // DI: sets I=0
    dut->i_di_set = 1;
    tick(dut);
    clear_inputs(dut);
    check1("di_sr_i", dut->o_sr_i, 0);

    // ── Priority: except_entry > sr_load ───────────────────────
    // If both assert, except_entry wins
    reset(dut);
    // Set up: user mode, I=1
    dut->i_sr_load = 1;
    dut->i_wdata   = (1u << SR_I);
    tick(dut);
    clear_inputs(dut);

    // Assert both except_entry and sr_load
    dut->i_except_entry = 1;
    dut->i_sr_load      = 1;
    dut->i_wdata        = (1u << SR_N) | (1u << SR_Z);  // would set N,Z, clear S,I
    tick(dut);
    clear_inputs(dut);
    // except_entry should win: S=1, I=0
    check1("prio_except_s", dut->o_sr_s, 1);
    check1("prio_except_i", dut->o_sr_i, 0);
    // Flags should be unchanged (except_entry doesn't touch them)
    check1("prio_except_n", dut->o_flag_n, 0);

    // ── Priority: sr_load > flag_w_en ──────────────────────────
    reset(dut);
    dut->i_sr_load    = 1;
    dut->i_wdata      = (1u << SR_Z) | (1u << SR_S);  // Z=1, S=1
    dut->i_flag_w_en  = 1;
    dut->i_alu_flag_n = 1;
    dut->i_alu_flag_z = 0;
    tick(dut);
    clear_inputs(dut);
    // sr_load wins: Z=1 (from wdata), not N=1 (from ALU)
    check1("prio_load_z", dut->o_flag_z, 1);
    check1("prio_load_n", dut->o_flag_n, 0);

    // ── Full RTI round-trip ────────────────────────────────────
    // Simulate: user code running → exception → handler → RTI
    reset(dut);

    // Step 1: Set up user state (S=0, I=1, N=1, Z=1, C=0, V=1)
    uint32_t user_sr = (1u << SR_N) | (1u << SR_Z) | (1u << SR_V) | (1u << SR_I);
    dut->i_sr_load = 1;
    dut->i_wdata   = user_sr;
    tick(dut);
    clear_inputs(dut);
    check("rti_user_state", dut->o_sr_read, user_sr);

    // Step 2: Exception entry
    dut->i_except_entry = 1;
    tick(dut);
    clear_inputs(dut);
    uint32_t saved_sr = dut->o_shadow_sr;
    check("rti_shadow_captured", saved_sr, user_sr);
    // Now in supervisor mode, interrupts off
    check1("rti_except_s", dut->o_sr_s, 1);
    check1("rti_except_i", dut->o_sr_i, 0);

    // Step 3: Handler modifies flags (e.g., CMP instruction)
    dut->i_flag_w_en  = 1;
    dut->i_alu_flag_n = 0;
    dut->i_alu_flag_z = 0;
    dut->i_alu_flag_c = 1;
    dut->i_alu_flag_v = 0;
    tick(dut);
    clear_inputs(dut);
    // Flags are now different from the saved user state
    check1("rti_handler_n", dut->o_flag_n, 0);
    check1("rti_handler_c", dut->o_flag_c, 1);

    // Step 4: RTI — restore SR from saved value (via sr_load)
    dut->i_sr_load = 1;
    dut->i_wdata   = saved_sr;
    tick(dut);
    clear_inputs(dut);
    // Should be back to original user state
    check("rti_restored", dut->o_sr_read, user_sr);
    check1("rti_restored_s", dut->o_sr_s, 0);  // back to user mode
    check1("rti_restored_i", dut->o_sr_i, 1);  // interrupts re-enabled

    // ── ei_shadow_clr is independent of priority chain ─────────
    // ei_shadow_clr should work even during exception entry
    reset(dut);
    dut->i_ei_set = 1;
    tick(dut);
    clear_inputs(dut);
    check1("indep_shadow_armed", dut->o_ei_shadow, 1);

    // Exception entry + ei_shadow_clr simultaneously
    dut->i_except_entry  = 1;
    dut->i_ei_shadow_clr = 1;
    tick(dut);
    clear_inputs(dut);
    check1("indep_shadow_clr_during_except", dut->o_ei_shadow, 0);

    // ── Summary ────────────────────────────────────────────────
    printf("status_reg: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}
