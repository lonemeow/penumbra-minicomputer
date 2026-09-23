# Non-project Vivado build for one FPGA top, from the Verilog the
# shared sv2v step writes to a bitstream (the xc7 half of `make fpga`).
#
# Usage: vivado -mode batch -source build.tcl
#            -tclargs <top module> <part> <out> <verilog> [<xdc> ...]
#
# <out> is the artifact prefix (build/<top>). Outputs:
#   <out>.bit                   the bitstream
#   <out>_timing_summary.rpt    read by `make timing`
#   <out>_utilization.rpt, <out>_drc.rpt
#   <out>_routed.dcp            for interactive timing analysis
# Reports come before the bitstream, so a failed bitgen still leaves
# them to read. Vivado's own scratch (.Xil/, clockInfo.txt) goes to
# <out>.vivado/ rather than the directory make runs in.

lassign $argv top part out verilog
set xdcs [lrange $argv 4 end]

set out     [file normalize $out]
set verilog [file normalize $verilog]
set xdcs    [lmap f $xdcs {file normalize $f}]
file mkdir $out.vivado
cd $out.vivado

read_verilog $verilog
foreach f $xdcs {
    read_xdc $f
}
synth_design -top $top -part $part

opt_design -directive Explore
place_design -directive Explore
phys_opt_design -directive Explore
route_design -directive Explore
phys_opt_design -directive Explore

report_timing_summary -max_paths 10 -report_unconstrained \
    -file ${out}_timing_summary.rpt
report_utilization -file ${out}_utilization.rpt
report_drc -file ${out}_drc.rpt
write_checkpoint -force ${out}_routed.dcp
write_bitstream -force ${out}.bit
