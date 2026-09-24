# Non-project Vivado build for one FPGA top, from the Verilog the
# shared sv2v step writes to a bitstream (the xc7 half of `make fpga`).
#
# Usage: vivado -mode batch -source build.tcl
#            -tclargs <top module> <part> <out> <log> <verilog> [<xdc> ...]
#
# <out> is the artifact prefix (build/<top>), and <log> the file given
# to vivado -log, which the critical-warning gate reads. Outputs:
#   <out>.bit                   the bitstream, only when every gate passes
#   <out>_timing_summary.rpt    read by `make timing`
#   <out>_utilization.rpt, <out>_drc.rpt, <out>_methodology.rpt,
#   <out>_cdc.rpt
#   <out>_routed.dcp            for interactive timing analysis
# The signoff gates (gates.tcl) run on the routed design; a failure
# exits non-zero without a bitstream, after the reports and checkpoint
# are written. Vivado's own scratch (.Xil/, clockInfo.txt) goes to
# <out>.vivado/ rather than the directory make runs in.

lassign $argv top part out log verilog
set xdcs [lrange $argv 5 end]

source [file join [file dirname [file normalize [info script]]] gates.tcl]

set out     [file normalize $out]
set log     [file normalize $log]
set verilog [file normalize $verilog]
set xdcs    [lmap f $xdcs {file normalize $f}]
set srcdir  [pwd]
file mkdir $out.vivado
cd $out.vivado

# A failed build must not leave an older bitstream behind to program.
file delete -force $out.bit

# Vivado resolves a $readmemh/$readmemb file name against its working
# directory, the scratch directory above, and synthesizes a memory whose
# image it cannot open as all zeros. Each image the design names is
# copied in from where make runs, so a missing one stops the build here.
set f [open $verilog r]
set text [read $f]
close $f
foreach {- image} [regexp -all -inline {\$readmem[hb]\s*\(\s*"([^"]+)"} $text] {
    if {[file pathtype $image] eq "relative"} {
        file mkdir [file dirname $image]
        file copy -force [file join $srcdir $image] $image
    }
}

read_verilog $verilog
foreach f $xdcs {
    read_xdc $f
}
synth_design -top $top -part $part

# Placement and routing run at their default effort; Explore costs them
# minutes a build, where the steps around them take seconds at it.
opt_design -directive Explore
place_design
phys_opt_design -directive Explore
route_design
phys_opt_design -directive Explore

report_timing_summary -max_paths 10 -report_unconstrained \
    -file ${out}_timing_summary.rpt
report_utilization -file ${out}_utilization.rpt
report_drc -file ${out}_drc.rpt
report_methodology -file ${out}_methodology.rpt
report_cdc -details -file ${out}_cdc.rpt
write_checkpoint -force ${out}_routed.dcp

set failures [gate_failures $log]
foreach line [clock_fmax_lines] {
    puts "Timing: $line"
}
if {[llength $failures] > 0} {
    foreach f $failures {
        puts "GATE FAILED: $f"
    }
    puts "Signoff: FAIL, no bitstream written"
    exit 1
}
puts "Signoff: PASS"

write_bitstream -force ${out}.bit
