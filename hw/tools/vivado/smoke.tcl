# Image-build check that this Vivado can do the Nexys Video job: the
# pinned release takes a trivial design for our exact part through
# synthesis, implementation and bitstream. A wrong installer, a missing
# device family or a license prompt then fails the image build instead
# of the first real one.
#
# Usage: vivado -mode batch -source smoke.tcl -tclargs <release>
# Run it from a scratch directory; every output lands there.

lassign $argv expected_release
set part xc7a200tsbg484-1

if {[version -short] ne $expected_release} {
    error "installer is Vivado [version -short]; the image pins $expected_release"
}
if {[llength [get_parts -quiet $part]] == 0} {
    error "part $part is not installed; is Artix-7 selected in install_config.txt?"
}

# A counter on the 100 MHz oscillator, blinking an LED: enough logic for
# placement, routing, timing analysis and bitgen to all do real work.
set fh [open smoke.v w]
puts $fh {
module smoke (
    input  wire clk,
    output wire led
);
    reg [25:0] count = 26'd0;
    always @(posedge clk) count <= count + 26'd1;
    assign led = count[25];
endmodule
}
close $fh

read_verilog smoke.v
synth_design -top smoke -part $part

# Pins and IOSTANDARDs from Digilent's Nexys Video master XDC, so the
# bitgen DRC sees fully constrained IO rather than waiving it.
set_property -dict {PACKAGE_PIN R4 IOSTANDARD LVCMOS33} [get_ports clk]
set_property -dict {PACKAGE_PIN T14 IOSTANDARD LVCMOS25} [get_ports led]
create_clock -name sys_clk -period 10.000 [get_ports clk]

opt_design
place_design
route_design
write_bitstream -force smoke.bit

# An unconstrained path reports an empty or infinite slack, and Tcl's
# expr reads "inf" as a number, so a plain `< 0` test would pass with
# nothing having been timed. Demand a finite slack.
proc worst_slack {analysis} {
    set slack [get_property SLACK [get_timing_paths -max_paths 1 -$analysis]]
    if {![string is double -strict $slack] || [string match -nocase *inf* $slack]} {
        error "no timed $analysis path (slack '$slack'); did create_clock apply?"
    }
    return $slack
}

set wns [worst_slack setup]
set whs [worst_slack hold]

if {$wns < 0} {
    error "setup timing failed, slack $wns"
}

if {$whs < 0} {
    error "hold timing failed, slack $whs"
}

set critical [get_msg_config -count -severity {CRITICAL WARNING}]
if {$critical > 0} {
    error "the run produced $critical critical warnings; see the messages above"
}

if {![file exists smoke.bit] || [file size smoke.bit] == 0} {
    error "no bitstream written"
}
