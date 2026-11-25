// penumbra4_landing.h — the ops gen4's machine lands, read and named.
//
// machine_penumbra4_sim's o_landing_* ports carry the ops whose effects
// land each cycle: one that retires, or one whose trap is taken. Each
// slot's op sits at its slot_e index (X = 0, Y = 1), and o_landing_older
// names the first in program order when both land. The program runner
// keeps the last few for its failure report; the console shim writes them
// as its trace.
//
// The name tables restate penumbra4_pkg's op_class_e and penumbra_pkg's
// VEC_* in their order, and the field widths are those packages' too:
// either changing there has to change here.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "Vmachine_penumbra4_sim.h"

struct Landing {
    uint64_t cycle;
    uint32_t pc;
    unsigned op_class;
    bool     trap;
    unsigned vector;    // meaningful when trap
};

static const char* const landing_op_class_names[] = {
    "ALU",   "MEM",   "BRANCH",   "FUNIT", "RDSPR", "WRSPR",
    "RDSYS", "WRSYS", "IRQ_MASK", "ERET",  "TRAP",
};

static const char* const landing_vector_names[] = {
    "BUS_FAULT", "TIMER", "TLB_MISS", "TLB_PROT", "PRIV",  "SYSCALL",
    "BREAK",     "ILLEGAL", "ALIGN",  "EXT_IRQ",  "ARITH",
};

template <size_t N>
static inline const char* landing_name(const char* const (&names)[N], unsigned i) {
    return i < N ? names[i] : "?";
}

// Physical registers (penumbra4_pkg's phys_reg_t): R0..R13, then the two
// banks of R14.
static inline const char* landing_phys_reg_name(unsigned phys) {
    static const char* const names[16] = {
        "R0", "R1", "R2",  "R3",  "R4",  "R5",  "R6",  "R7",
        "R8", "R9", "R10", "R11", "R12", "R13", "USP", "SSP",
    };
    return names[phys & 0xF];
}

// The ops landing this cycle, in program order. Returns how many (0..2).
static inline int read_landings(const Vmachine_penumbra4_sim* dut, uint64_t cycle,
                                Landing out[2]) {
    const unsigned PC_BITS = 32, CLASS_BITS = 4, VECTOR_BITS = 4;
    const unsigned older = dut->o_landing_older & 1u;
    const unsigned order[2] = { older, older ^ 1u };
    int n = 0;
    for (unsigned slot : order) {
        if (!((dut->o_landing_valid >> slot) & 1u)) continue;
        Landing& l = out[n++];
        l.cycle    = cycle;
        l.pc       = uint32_t(dut->o_landing_pc >> (PC_BITS * slot));
        l.op_class = (dut->o_landing_op_class >> (CLASS_BITS * slot)) & 0xFu;
        l.trap     = (dut->o_landing_trap >> slot) & 1u;
        l.vector   = (dut->o_landing_vector >> (VECTOR_BITS * slot)) & 0xFu;
    }
    return n;
}

// "cyc=… PC=… MEM", and for a trap "cyc=… PC=… MEM      trap ALIGN".
static inline std::string format_landing(const Landing& l) {
    char m[96];
    if (l.trap) {
        snprintf(m, sizeof(m), "cyc=%lu PC=%08x %-8s trap %s",
                 (unsigned long)l.cycle, l.pc,
                 landing_name(landing_op_class_names, l.op_class),
                 landing_name(landing_vector_names, l.vector));
    } else {
        snprintf(m, sizeof(m), "cyc=%lu PC=%08x %s",
                 (unsigned long)l.cycle, l.pc,
                 landing_name(landing_op_class_names, l.op_class));
    }
    return std::string(m);
}
