// tb_nexysvideo_penumbra2_top — the Nexys Video board testbench
// (nexysvideo_top_tb.h) over the gen2 machine's top:
//   make -C hw/rom && make font8x16.hex && make sim MOD=nexysvideo_penumbra2_top

#include "Vnexysvideo_penumbra2_top.h"
#include <cstdint>

using NexysTop = Vnexysvideo_penumbra2_top;
static const uint64_t CPU_HZ = 60000000;   // the top's CPU_HZ

#include "nexysvideo_top_tb.h"
