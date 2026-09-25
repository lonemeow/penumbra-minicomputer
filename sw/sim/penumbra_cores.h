// penumbra_cores.h — the hardware generations the ISS can present.
//
// Software tells the generations apart by two things: the CPU name
// (SYSDEV_CPU NAME0..3) and the geometry each cache's INFO register
// reports.  Everything else the ISS models — the ISA, the TLB and its
// sysreg interface, the devices — is common to every generation.  A
// generation is named the way `make CORE=` names it.
//
// The values mirror the RTL parameters of each machine, so a change to a
// generation's cache geometry or CPU name there has to land here too.

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

struct CoreGeneration {
    const char*   name;        // as `make CORE=` spells it
    uint32_t      cpu_name2;   // CPU name word 2: the generation tag
    CacheGeometry l1i, l1d, l2;
};

static const CoreGeneration CORE_GENERATIONS[] = {
    // cpu_core's two cache_vipt instances at their defaults (64 sets,
    // direct-mapped, 4-word lines) and l2_cache at its defaults (64 KiB,
    // 4 ways, 16-byte lines).  cpuid's default name, "Penumbra/1".
    { "penumbra1", 0x0000312F,
      { 64, 1, 16, CACHE_ADDRESSING_VIPT },
      { 64, 1, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT } },

    // machine_penumbra2's two cache_bram_vipt instances (4 KiB, 2 ways)
    // and the same l2_cache.  The name tags the variant: "Penumbra/2" for
    // the baseline, "Penumbra/2.5" for the forwarding/prediction variant.
    { "penumbra2", 0x0000322F,
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT } },
    { "penumbra2_5", 0x352E322F,
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 128, 2, 16, CACHE_ADDRESSING_VIPT },
      { 1024, 4, 16, CACHE_ADDRESSING_PIPT } },
};

// The generation the ISS presents unless told otherwise: the identity it
// has always claimed.
static const char* const DEFAULT_CORE_GENERATION = "penumbra1";

inline const CoreGeneration* find_core_generation(const char* name) {
    for (const CoreGeneration& g : CORE_GENERATIONS)
        if (strcmp(g.name, name) == 0) return &g;
    return nullptr;
}
