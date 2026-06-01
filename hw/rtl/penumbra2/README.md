# Penumbra/2 RTL

The gen2 CPU core (6-stage pipelined, hardwired control) lands here.
The design is specified under
[`doc/internals/penumbra2/`](../../../doc/internals/penumbra2/); RTL
implementation has not started.

The shared ISA-constants package lives at
[`hw/rtl/common/penumbra_pkg.sv`](../common/penumbra_pkg.sv) and is
used by both generations. The gen1 core is in
[`hw/rtl/penumbra1/`](../penumbra1/). The surrounding system
(`mmu/`, `soc/`, `io/`, `bus/`) is shared unchanged.
