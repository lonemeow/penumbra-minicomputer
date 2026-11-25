# Non-project Vivado build for one FPGA top, from the Verilog the
# shared sv2v step writes to a bitstream (the xc7 half of `make fpga`).
#
# Usage: vivado -mode batch -source build.tcl
#            -tclargs <top module> <part> <out> <log> <verilog> [<xdc> ...]
#
# <out> is the artifact prefix (build/<top>), and <log> the file given
# to vivado -log, which the critical-warning gate reads. Outputs:
#   <out>.bit                   the bitstream, only when every gate passes
#   <out>_summary.txt           the run's verdict, each clock's fmax, the
#                               worst paths and the main resources' use,
#                               also printed at the end of the run
#   <out>_timing_summary.rpt    read by `make timing`
#   <out>_fanout.rpt            the widest nets, by the pins they drive
#   <out>_control_sets.rpt      every clock, enable and reset combination
#                               and the flops sharing it
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

# A failed build must not leave an older bitstream behind to program,
# nor an older run's summary to read as this one's.
file delete -force $out.bit ${out}_summary.txt

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

# The XADC's over-temperature shutdown acts only if the bitstream allows
# it; the thermal gate checks that every design arms it (gates.tcl).
set_property BITSTREAM.CONFIG.OVERTEMPPOWERDOWN ENABLE [current_design]

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
report_high_fanout_nets -load_types -timing -max_nets 50 -file ${out}_fanout.rpt
report_control_sets -verbose -file ${out}_control_sets.rpt
report_drc -file ${out}_drc.rpt
report_methodology -file ${out}_methodology.rpt
report_cdc -details -file ${out}_cdc.rpt
write_checkpoint -force ${out}_routed.dcp

# The summary: what is worth reading after every run, kept apart from
# Vivado's own output.
set failures [gate_failures $log]
set summary {}
foreach line [clock_fmax_lines] {
    lappend summary "Timing: $line"
}
foreach line [worst_path_lines 10] {
    lappend summary "Path: $line"
}
foreach line [utilization_lines] {
    lappend summary "Utilization: $line"
}
foreach line [thermal_lines] {
    lappend summary "Thermal: $line"
}
foreach f $failures {
    lappend summary "GATE FAILED: $f"
}
if {[llength $failures] > 0} {
    lappend summary "Signoff: FAIL, no bitstream written"
} else {
    lappend summary "Signoff: PASS"
}
set f [open ${out}_summary.txt w]
foreach line $summary {
    puts $line
    puts $f $line
}
close $f
if {[llength $failures] > 0} {
    exit 1
}

write_bitstream -force ${out}.bit
