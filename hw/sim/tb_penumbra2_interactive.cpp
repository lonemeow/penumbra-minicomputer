// tb_penumbra2_interactive.cpp — Penumbra/2 shim for the shared sim console.
//
// Wraps Vmachine_penumbra2_sim behind the SimCore interface: a CPU clock plus a
// faster i_sdram_clk for the full SDRAM model, and BREAK observed via o_prog_end (gen2
// traps a retiring BREAK rather than driving an o_halted line). It reconstructs
// an instruction trace from the machine's commit/retire observability ports —
// gen2 is a pipeline with a 2R/1W regfile and no spare debug read port, so
// unlike gen1 there is no register-dump loop: each retiring instruction emits a
// line from o_retire_* + the o_commit_* write port. All terminal / SD / stdin /
// exit logic lives in sim_console.cpp.
//
// Usage: make simulate-rtl CORE=penumbra2   (TRACE=trace.log for the trace)

#include "verilated.h"
#include "Vmachine_penumbra2_sim.h"
#include "sim_console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Pipeline-occupancy trace window (PCs of interest), set from +pipe_lo=/+pipe_hi=
// in main(). Zero hi = disabled (the trace is high volume, so it is opt-in).
static uint32_t g_pipe_lo = 0;
static uint32_t g_pipe_hi = 0;

// op_class (penumbra2_pkg OPC_*) -> mnemonic, indexed by the 4-bit retire code.
static const char* const opc_names[16] = {
    "ALU",    "LOAD",   "STORE", "BRANCH",
    "JMP",    "DIVMUL", "RDSPR", "WRSPR",
    "RDSYS",  "WRSYS",  "ERET",  "EI",
    "DI",     "SYSCALL","BREAK", "ILLEGAL",
};

// Vector number -> name, mirroring penumbra_pkg.sv (VEC_BUS_FAULT=0 .. EXT_IRQ=9).
static const char* const vec_names[16] = {
    "BUS_FAULT", "TIMER",   "TLB_MISS", "TLB_PROT",
    "PRIV",      "SYSCALL", "BREAK",    "ILLEGAL",
    "ALIGN",     "EXT_IRQ", "rsvd10",   "rsvd11",
    "rsvd12",    "rsvd13",  "rsvd14",   "rsvd15",
};

// phys_reg_name — label a physical scoreboard entry for the trace.
//
// The gen2 regfile is *physically* addressed (penumbra2_regmap.sv): the commit
// port's o_commit_idx is a scoreboard entry, not an architectural register
// number. The mapping (penumbra2_pkg.sv SB_*):
//     0..13  -> arch GPRs R0..R13
//     14     -> R14 user bank (USP)      15 -> R14 supervisor bank (SSP)
//     16 ESR   17 EPC   18..21 SCR0..SCR3
// Return a short label for `idx` — e.g. "R3", a disambiguated form for the two
// R14 banks, and the SPR names for 16..21. How you render the banks and SPRs
// is the call to make; a static buffer (snprintf) or a switch both work. This
// runs once per committing instruction in the trace, so keep it allocation-free.
static const char* phys_reg_name(unsigned idx) {
    switch (idx) {
        case 0: return "R0";
        case 1: return "R1";
        case 2: return "R2";
        case 3: return "R3";
        case 4: return "R4";
        case 5: return "R5";
        case 6: return "R6";
        case 7: return "R7";
        case 8: return "R8";
        case 9: return "R9";
        case 10: return "R10";
        case 11: return "R11";
        case 12: return "R12";
        case 13: return "R13";
        case 14: return "USP";
        case 15: return "SSP";
        case 16: return "ESR";
        case 17: return "EPC";
        case 18: return "SCR0";
        case 19: return "SCR1";
        case 20: return "SCR2";
        case 21: return "SCR3";
        default: return "**INVALID**";
    }
}

// Compose the "cyc=… PC=… OPC SR=… [flags]" prefix shared by a WB retirement
// and an EX drain-commit. Returns the snprintf offset so the caller can append
// (e.g. a register write). op_class indexes opc_names; sr supplies the flags.
static int fmt_insn_prefix(char* line, size_t n, uint64_t cycle,
                           uint32_t pc, unsigned op_class, uint32_t sr) {
    return snprintf(line, n,
            "cyc=%lu PC=%08x %-7s SR=%08x [%c%c%c%c %c%c]",
            (unsigned long)cycle, pc, opc_names[op_class & 0xF], sr,
            (sr & (1u <<  0)) ? 'N' : '-',
            (sr & (1u <<  1)) ? 'Z' : '-',
            (sr & (1u <<  2)) ? 'C' : '-',
            (sr & (1u <<  3)) ? 'V' : '-',
            (sr & (1u << 31)) ? 'S' : 'u',
            (sr & (1u << 30)) ? 'I' : '-');
}

// Format a trap-entry ("ENTER") or ERET marker. vector is meaningful only for
// ENTER; pass -1 for ERET.
static std::string fmt_trap_marker(const char* kind, uint64_t cycle,
                                   uint32_t pc, uint32_t sr, int vector) {
    char m[256];
    const char* vec_name = (vector != -1) ? vec_names[vector & 0xF] : "N/A";
    snprintf(m, sizeof(m), "[TRAP %s cyc=%lu pc=%08x vec=%s S=%d I=%d]\n",
             kind, (unsigned long)cycle, pc, vec_name,
             !!(sr & 0x80000000u), !!(sr & 0x40000000u));
    return std::string(m);
}

struct Penumbra2Core : SimCore {
    Vmachine_penumbra2_sim* dut = new Vmachine_penumbra2_sim;
    bool rx_ack_latched = false;

    ~Penumbra2Core() override { delete dut; }

    void reset() override {
        dut->i_rst = 1;
        dut->i_irq = 0;
        dut->i_timer_irq = 0;
        dut->i_uart_rx_valid = 0;
        dut->i_uart_rx_data = 0;
        dut->i_spi_resp_valid = 0;
        dut->i_spi_resp_data = 0xFF;
        dut->i_sdram_clk = 0;
        tick();   // two reset cycles
        tick();
        dut->i_rst = 0;
    }

    // One CPU cycle with the dual SDRAM clock: 4 SDRAM half-cycles per CPU
    // half-cycle (hardware's 25 MHz CPU / 100 MHz SDRAM ratio). o_uart_rx_ack is
    // sampled at the falling edge, where the sim UART's combinational ack is
    // valid, before the SDRAM toggles.
    void tick() override {
        dut->i_clk = 0;
        dut->eval();
        rx_ack_latched = dut->o_uart_rx_ack;
        for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
        dut->i_clk = 1;
        dut->eval();
        for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
    }

    void uart_rx(bool v, uint8_t d) override { dut->i_uart_rx_valid = v; dut->i_uart_rx_data = d; }
    bool uart_rx_ack() const override { return rx_ack_latched; }
    bool uart_tx(uint8_t& d) const override {
        if (dut->o_uart_tx_valid) { d = dut->o_uart_tx_data; return true; }
        return false;
    }
    bool spi_cs0() const override { return dut->o_spi_cs0; }
    bool spi_cmd(uint8_t& d) const override {
        if (dut->o_spi_cmd_valid) { d = dut->o_spi_cmd_data; return true; }
        return false;
    }
    void spi_resp(bool v, uint8_t d) override { dut->i_spi_resp_valid = v; dut->i_spi_resp_data = d; }

    bool     halted() const override { return dut->o_prog_end; }
    // At the program-end pulse the retiring BREAK is the MEM/WB slot, so its PC
    // is on o_retire_pc; between retirements this holds the last retired PC.
    uint32_t pc()     const override { return dut->o_retire_pc; }

    bool trace_supported() const override { return true; }

    // Per-cycle pipeline occupancy, gated by a PC window (+pipe_lo=/+pipe_hi=,
    // off by default — it is high volume). A line is emitted each cycle an
    // instruction in [lo,hi] occupies EX, MEM, or WB, so a slot dropped between
    // stages is directly visible (e.g. a branch that resolves in EX but never
    // reaches WB). WB rides the retire port; EX/MEM come from o_ex_*/o_mem_*.
    void pipe_occupancy(std::vector<std::string>& out, uint64_t cycle) {
        if (!g_pipe_hi) return;   // disabled
        auto inwin = [](uint32_t pc, bool v){ return v && pc >= g_pipe_lo && pc <= g_pipe_hi; };
        bool exv = dut->o_ex_valid, memv = dut->o_mem_valid, wbv = dut->o_retire_valid;
        if (!(inwin(dut->o_ex_pc, exv) || inwin(dut->o_mem_pc, memv) || inwin(dut->o_retire_pc, wbv)))
            return;
        char ex[9], mem[9], wb[9];
        if (exv)  snprintf(ex,  9, "%08x", dut->o_ex_pc);     else strcpy(ex,  "--------");
        if (memv) snprintf(mem, 9, "%08x", dut->o_mem_pc);    else strcpy(mem, "--------");
        if (wbv)  snprintf(wb,  9, "%08x", dut->o_retire_pc); else strcpy(wb,  "--------");
        char m[160];
        snprintf(m, sizeof(m), "occ cyc=%lu EX=%s MEM=%s WB=%s\n",
                 (unsigned long)cycle, ex, mem, wb);
        out.push_back(std::string(m));
    }

    void trace(std::vector<std::string>& out, uint64_t cycle) override {
        pipe_occupancy(out, cycle);
        // One line per retiring instruction. A WB retirement (o_retire_valid)
        // and an EX drain-commit (o_dc_commit) are mutually exclusive per cycle
        // (asserted in penumbra2_core), so they form a single contiguous PC
        // stream — without the drain-commit leg, EI/DI/WRSYS/ERET would leave
        // gaps over sysreg/mode-change code.
        if (dut->o_retire_valid) {
            char line[256];
            int off = fmt_insn_prefix(line, sizeof(line), cycle, dut->o_retire_pc,
                                      dut->o_retire_op_class, dut->o_retire_sr);
            if (dut->o_commit_we)
                off += snprintf(line + off, sizeof(line) - off, " %s=%08x",
                                phys_reg_name(dut->o_commit_idx), dut->o_commit_data);
            if (off < (int)sizeof(line) - 1) line[off++] = '\n';
            line[off] = '\0';
            out.push_back(std::string(line));
        } else if (dut->o_dc_commit) {
            // Drain-commit: retires from EX, writes no GPR. SR is the current
            // committed word (the same pre-own-update view as a WB retire).
            char line[256];
            int off = fmt_insn_prefix(line, sizeof(line), cycle, dut->o_dc_commit_pc,
                                      dut->o_dc_commit_op_class, dut->o_retire_sr);
            if (off < (int)sizeof(line) - 1) line[off++] = '\n';
            line[off] = '\0';
            out.push_back(std::string(line));
        }
        // Branch resolution: the EX branch resolve that steers the front end.
        // A line here whose pc never appears as a retire means the branch
        // redirected fetch but was then squashed before WB — invisible to the
        // retire stream alone.
        if (dut->o_branch_taken) {
            char m[128];
            snprintf(m, sizeof(m), "[BR taken cyc=%lu pc=%08x -> %08x]\n",
                     (unsigned long)cycle, dut->o_branch_pc, dut->o_branch_target);
            out.push_back(std::string(m));
        }
        // Trap markers. gen2's vector is valid the same cycle as the commit
        // pulse (no gen1-style deferral); the faulting/returning instruction's
        // PC is the retiring slot's PC.
        if (dut->o_fault_commit)
            out.push_back(fmt_trap_marker("ENTER", cycle, dut->o_retire_pc,
                                          dut->o_retire_sr, dut->o_fault_vec));
        if (dut->o_eret_commit)
            out.push_back(fmt_trap_marker("ERET", cycle, dut->o_retire_pc,
                                          dut->o_retire_sr, -1));
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    // Optional pipeline-occupancy window (debug): +pipe_lo=<pc> +pipe_hi=<pc>.
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "+pipe_lo=", 9) == 0) g_pipe_lo = (uint32_t)strtoul(argv[i] + 9, nullptr, 0);
        else if (strncmp(argv[i], "+pipe_hi=", 9) == 0) g_pipe_hi = (uint32_t)strtoul(argv[i] + 9, nullptr, 0);
    }
    Penumbra2Core core;
    return run_console(core, parse_console_opts(argc, argv));
}
