// penumbra_cores.h — the hardware generations the ISS can present.
//
// Software tells the generations apart by the CPU name (SYSDEV_CPU
// NAME0..3), the geometry each cache's INFO register reports, and the
// main TLB's geometry and the MMU's interface version, which MMU_CONFIG
// reports.  Everything else the ISS models — the ISA, the devices — is
// common to every generation.  A generation is named the way `make
// CORE=` names it.
//
// A generation also carries the multiplier bits its divmul unit retires
// a step, which software cannot see and +opstats counts steps by.
//
// The values mirror the RTL parameters of each machine, so a change to a
// generation's cache or TLB geometry, MMU version, CPU name or multiplier
// step there has to land here too.

#pragma once

#include <cstdint>
#include <cstring>

// Cache addressing, as INFO[27:26] encodes it (CACHE_ADDR_* in
// penumbra_pkg.sv).
enum : uint32_t { CACHE_ADDRESSING_PIPT = 0, CACHE_ADDRESSING_VIPT = 1 };

// One cache as its INFO register describes it.  Every cache so far is
// write-through and write-no-allocate, so those INFO bits read 0.
struct CacheGeometry {
    uint32_t sets;
    uint32_t ways;
    uint32_t line_bytes;
    uint32_t addressing;

    uint32_t bytes() const { return sets * ways * line_bytes; }

    // The unified cache INFO layout: line_words[5:0] | sets[20:6] |
    // ways[25:21] | addressing[27:26] | write-back[28] | write-alloc[29].
    uint32_t info() const {
        return (addressing << 26) | (ways << 21) | (sets << 6) | (line_bytes / 4);
    }
};

// The main TLB: sets indexed by the VPN's low bits, software choosing
// the way on a refill.
struct TlbGeometry {
    uint32_t sets;
    uint32_t ways;
};

struct CoreGeneration {
    const char*   name;        // as `make CORE=` spells it
    uint32_t      cpu_name2;   // CPU name word 2: the generation tag
    CacheGeometry l1i, l1d, l2;
    TlbGeometry   tlb;
    uint32_t      mmu_version;         // MMU_CONFIG.VER, its TLB interface
    uint32_t      mul_bits_per_step;   // divmul's MUL_BITS_PER_STEP
};

static const CoreGeneration CORE_GENERATIONS[] = {
    // cpu_core's two cache_vipt instances at their defaults (64 sets,
    // direct-mapped, 4-word lines) and l2_cache at its defaults (64 KiB,
    // 4 ways, 16-byte lines); tlb's 64 entries, 2-way, behind MMU
    // interface version 1.  cpuid's default name, "Penumbra/1".  datapath's divmul at its default of one
    // multiplier bit a step.
    { "penumbra1", 0x0000312F,
      { 64, 1, 16, CACHE_ADDRESSING_VIPT },
      { 64, 1, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT },
      { 32, 2 }, 1,
      1 },

    // machine_penumbra2's two cache_bram_vipt instances (4 KiB, 2 ways),
    // the same l2_cache, and penumbra2_tlb's 64 entries, 2-way, behind MMU
    // interface version 1.  The name
    // tags the variant: "Penumbra/2" for the baseline, "Penumbra/2.5" for
    // the forwarding/prediction variant.  Both EX stages build divmul at
    // four multiplier bits a step.
    { "penumbra2", 0x0000322F,
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT },
      { 32, 2 }, 1,
      4 },
    { "penumbra2_5", 0x352E322F,
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT },
      { 32, 2 }, 1,
      4 },

    // machine_penumbra3 at its default geometry: two penumbra3_l1 instances
    // of 2 ways, a page a way, and penumbra3_uncore's L2 at 1024 sets of 4
    // ways; penumbra3_mmu's 32 sets of 2 ways, behind MMU interface version
    // 1.  "Penumbra/3".  Its EX stage builds divmul at four multiplier bits
    // a step.
    { "penumbra3", 0x0000332F,
      { 256, 2, 16, CACHE_ADDRESSING_VIPT },
      { 256, 2, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT },
      { 32, 2 }, 1,
      4 },
};

// The generation the ISS presents unless told otherwise: the identity it
// has always claimed.
static const char* const DEFAULT_CORE_GENERATION = "penumbra1";

inline const CoreGeneration* find_core_generation(const char* name) {
    for (const CoreGeneration& g : CORE_GENERATIONS)
        if (strcmp(g.name, name) == 0) return &g;
    return nullptr;
}
