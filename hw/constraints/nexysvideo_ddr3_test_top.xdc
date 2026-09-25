## nexysvideo_ddr3_test_top: board lines from the Nexys Video master XDC
## and the DDR3 pins from its MIG project

## Clock Signal
set_property -dict { PACKAGE_PIN R4    IOSTANDARD LVCMOS33 } [get_ports { clk }]; #IO_L13P_T2_MRCC_34 Sch=sysclk
create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports clk]

## LEDs
set_property -dict { PACKAGE_PIN T14   IOSTANDARD LVCMOS25 } [get_ports { led[0] }]; #IO_L15P_T2_DQS_13 Sch=led[0]
set_property -dict { PACKAGE_PIN T15   IOSTANDARD LVCMOS25 } [get_ports { led[1] }]; #IO_L15N_T2_DQS_13 Sch=led[1]
set_property -dict { PACKAGE_PIN T16   IOSTANDARD LVCMOS25 } [get_ports { led[2] }]; #IO_L17P_T2_13 Sch=led[2]
set_property -dict { PACKAGE_PIN U16   IOSTANDARD LVCMOS25 } [get_ports { led[3] }]; #IO_L17N_T2_13 Sch=led[3]
set_property -dict { PACKAGE_PIN V15   IOSTANDARD LVCMOS25 } [get_ports { led[4] }]; #IO_L14N_T2_SRCC_13 Sch=led[4]
set_property -dict { PACKAGE_PIN W16   IOSTANDARD LVCMOS25 } [get_ports { led[5] }]; #IO_L16N_T2_13 Sch=led[5]
set_property -dict { PACKAGE_PIN W15   IOSTANDARD LVCMOS25 } [get_ports { led[6] }]; #IO_L16P_T2_13 Sch=led[6]
set_property -dict { PACKAGE_PIN Y13   IOSTANDARD LVCMOS25 } [get_ports { led[7] }]; #IO_L5P_T0_13 Sch=led[7]
set_false_path -to [get_ports { led[*] uart_rx_out }]

## Buttons
set_property -dict { PACKAGE_PIN G4  IOSTANDARD LVCMOS15 } [get_ports { cpu_resetn }]; #IO_L12N_T1_MRCC_35 Sch=cpu_resetn

## UART
set_property -dict { PACKAGE_PIN AA19  IOSTANDARD LVCMOS33 } [get_ports { uart_rx_out }]; #IO_L15P_T2_DQS_RDWR_B_14 Sch=uart_rx_out

## DDR3, from hw/third_party/digilent-boards/nexys_video_mig.prj
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

## Configuration options, can be used for all designs
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property CFGBVS VCCO [current_design]

## Design constraints
# Bank 35's VREF pins carry DDR3 signals, so its SSTL15 inputs take their
# reference from inside the part: half the 1.5 V supply. The MIG project
# states it (InternalVref); without it, placement stops on DRC BIVRU-1.
set_property INTERNAL_VREF 0.750 [get_iobanks 35]

# The DRAM's data and strobes: split on-die termination on the FPGA's
# inputs, and fast edges on everything the controller drives, as the
# memory interface generator sets them for this bank.
set_property IN_TERM UNTUNED_SPLIT_50 [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]
set_property SLEW FAST [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] ddr3_dm[*] ddr3_addr[*] ddr3_ba[*] ddr3_ras_n ddr3_cas_n ddr3_we_n ddr3_cke[*] ddr3_odt[*] ddr3_reset_n }]

# The button is asynchronous to every clock in the design and enters it
# through two-flop synchronizers; the LEDs and the UART line go to a
# person and to a UART, neither of which samples them against our clock.
set_false_path -from [get_ports { cpu_resetn }]
set_false_path -to [get_ports { led[*] uart_rx_out }]

# The DRAM's pins are timed by the controller, not by static timing:
# every one of them is driven or captured by an I/O-cell serializer,
# whose clocks come from the same PLL, and the data and strobes pass
# delay lines calibration sets. Their board-level timing is what write
# leveling and read training measure and correct on every reset.
set_false_path -to [get_ports { ddr3_* }]
set_false_path -from [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]

# What the DRAM drives it drives in the DDR3 clock's domain: the chip
# answers the clock the FPGA sends it, and the serializers that capture
# its data run on that same clock. Saying so keeps the clock-crossing
# check from reading the inputs as asynchronous; that check follows each
# half of a strobe pair on its own, so both halves say it. The delay's
# value is never timed (the false path above).
set ddr3_clk [get_clocks -of_objects [get_pins u_clk/u_pll/CLKOUT1]]
set_input_delay -clock $ddr3_clk 0.0 [get_ports { ddr3_dq[*] ddr3_dqs_p[*] ddr3_dqs_n[*] }]
