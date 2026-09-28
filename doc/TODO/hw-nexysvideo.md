# Hardware: Nexys Video board

Work bound to the Nexys Video board.

## Build system: Vivado thread count

Vivado caps itself at 8 threads (`general.maxThreads`), while its
container sees every host core (16 on the development machine). On
the Nexys Video gen2.5 machine top, synthesis is effectively serial
(3:08 of CPU over 3:10 elapsed), so more threads could only speed up
place, route and the analysis steps. Open: once builds get large (the
DDR3 machine), time one build at 8 threads against one at 16; if place
and route shrink noticeably, pass a make variable through to
`hw/tools/vivado/build.tcl`, kept adjustable so a concurrent LLVM
build still gets its share of the cores.
