## The Nexys Video's DDR3, for every top carrying nexysvideo_ddr3 with
## its clocks from u_ddr3_clk (nexysvideo_ddr3_clk). Pins from Digilent's
## MIG project (hw/third_party/digilent-boards/nexys_video_mig.prj).

set_property -dict { PACKAGE_PIN P5 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_ck_p[0] }]
set_property -dict { PACKAGE_PIN P4 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_ck_n[0] }]
set_property -dict { PACKAGE_PIN G1 IOSTANDARD LVCMOS15 } [get_ports { ddr3_reset_n }]
set_property -dict { PACKAGE_PIN J6 IOSTANDARD SSTL15 } [get_ports { ddr3_cke[0] }]
set_property -dict { PACKAGE_PIN J4 IOSTANDARD SSTL15 } [get_ports { ddr3_ras_n }]
set_property -dict { PACKAGE_PIN K3 IOSTANDARD SSTL15 } [get_ports { ddr3_cas_n }]
set_property -dict { PACKAGE_PIN L1 IOSTANDARD SSTL15 } [get_ports { ddr3_we_n }]
set_property -dict { PACKAGE_PIN K4 IOSTANDARD SSTL15 } [get_ports { ddr3_odt[0] }]
set_property -dict { PACKAGE_PIN L3 IOSTANDARD SSTL15 } [get_ports { ddr3_ba[0] }]
set_property -dict { PACKAGE_PIN K6 IOSTANDARD SSTL15 } [get_ports { ddr3_ba[1] }]
set_property -dict { PACKAGE_PIN L4 IOSTANDARD SSTL15 } [get_ports { ddr3_ba[2] }]
set_property -dict { PACKAGE_PIN M2 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[0] }]
set_property -dict { PACKAGE_PIN M5 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[1] }]
set_property -dict { PACKAGE_PIN M3 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[2] }]
set_property -dict { PACKAGE_PIN M1 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[3] }]
set_property -dict { PACKAGE_PIN L6 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[4] }]
set_property -dict { PACKAGE_PIN P1 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[5] }]
set_property -dict { PACKAGE_PIN N3 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[6] }]
set_property -dict { PACKAGE_PIN N2 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[7] }]
set_property -dict { PACKAGE_PIN M6 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[8] }]
set_property -dict { PACKAGE_PIN R1 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[9] }]
set_property -dict { PACKAGE_PIN L5 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[10] }]
set_property -dict { PACKAGE_PIN N5 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[11] }]
set_property -dict { PACKAGE_PIN N4 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[12] }]
set_property -dict { PACKAGE_PIN P2 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[13] }]
set_property -dict { PACKAGE_PIN P6 IOSTANDARD SSTL15 } [get_ports { ddr3_addr[14] }]
set_property -dict { PACKAGE_PIN G3 IOSTANDARD SSTL15 } [get_ports { ddr3_dm[0] }]
set_property -dict { PACKAGE_PIN F1 IOSTANDARD SSTL15 } [get_ports { ddr3_dm[1] }]
set_property -dict { PACKAGE_PIN K2 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_dqs_p[0] }]
set_property -dict { PACKAGE_PIN E1 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_dqs_p[1] }]
set_property -dict { PACKAGE_PIN J2 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_dqs_n[0] }]
set_property -dict { PACKAGE_PIN D1 IOSTANDARD DIFF_SSTL15 } [get_ports { ddr3_dqs_n[1] }]
set_property -dict { PACKAGE_PIN G2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[0] }]
set_property -dict { PACKAGE_PIN H4 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[1] }]
set_property -dict { PACKAGE_PIN H5 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[2] }]
set_property -dict { PACKAGE_PIN J1 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[3] }]
set_property -dict { PACKAGE_PIN K1 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[4] }]
set_property -dict { PACKAGE_PIN H3 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[5] }]
set_property -dict { PACKAGE_PIN H2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[6] }]
set_property -dict { PACKAGE_PIN J5 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[7] }]
set_property -dict { PACKAGE_PIN E3 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[8] }]
set_property -dict { PACKAGE_PIN B2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[9] }]
set_property -dict { PACKAGE_PIN F3 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[10] }]
set_property -dict { PACKAGE_PIN D2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[11] }]
set_property -dict { PACKAGE_PIN C2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[12] }]
set_property -dict { PACKAGE_PIN A1 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[13] }]
set_property -dict { PACKAGE_PIN E2 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[14] }]
set_property -dict { PACKAGE_PIN B1 IOSTANDARD SSTL15 } [get_ports { ddr3_dq[15] }]

# Bank 35's VREF pins carry DDR3 signals, so its inputs take the internal
# reference, as the MIG project states; without it placement stops on
# DRC BIVRU-1.
set_property INTERNAL_VREF 0.750 [get_iobanks 35]

# The MIG's settings for this bank.
set_property IN_TERM UNTUNED_SPLIT_50 [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]
set_property SLEW FAST [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] ddr3_dm[*] ddr3_addr[*] ddr3_ba[*] ddr3_ras_n ddr3_cas_n ddr3_we_n ddr3_cke[*] ddr3_odt[*] ddr3_reset_n }]

# Every DRAM pin is an I/O-cell serializer's, and its board timing is
# what calibration trains, not what static timing checks.
set_false_path -to [get_ports { ddr3_* }]
set_false_path -from [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]

# The DRAM's outputs belong to the DDR3 clock's domain; without a clock
# the CDC check reads them as asynchronous. Each half of a strobe pair
# needs it. The delay itself is never timed (the false path above).
set ddr3_clk [get_clocks -of_objects [get_pins u_ddr3_clk/u_pll/CLKOUT1]]
set_input_delay -clock $ddr3_clk 0.0 [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]
