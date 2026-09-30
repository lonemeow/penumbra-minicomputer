## The Nexys Video machine tops, whatever their core: board lines from the
## master XDC (the DDR3's are in nexysvideo_ddr3.xdc). A top's clock
## managers are u_clk_rst (nexysvideo_clk_rst) and u_ddr3_clk
## (nexysvideo_ddr3_clk).

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

## Buttons
set_property -dict { PACKAGE_PIN G4  IOSTANDARD LVCMOS15 } [get_ports { cpu_resetn }]; #IO_L12N_T1_MRCC_35 Sch=cpu_resetn

## UART
set_property -dict { PACKAGE_PIN AA19  IOSTANDARD LVCMOS33 } [get_ports { uart_rx_out }]; #IO_L15P_T2_DQS_RDWR_B_14 Sch=uart_rx_out
set_property -dict { PACKAGE_PIN V18   IOSTANDARD LVCMOS33 } [get_ports { uart_tx_in }]; #IO_L14P_T2_SRCC_14 Sch=uart_tx_in

## SD card
set_property -dict { PACKAGE_PIN W19   IOSTANDARD LVCMOS33 } [get_ports { sd_cclk }]; #IO_L12P_T1_MRCC_14 Sch=sd_cclk
set_property -dict { PACKAGE_PIN W20   IOSTANDARD LVCMOS33 } [get_ports { sd_cmd }]; #IO_L12N_T1_MRCC_14 Sch=sd_cmd
set_property -dict { PACKAGE_PIN V19   IOSTANDARD LVCMOS33 } [get_ports { sd_d[0] }]; #IO_L14N_T2_SRCC_14 Sch=sd_d[0]
set_property -dict { PACKAGE_PIN T21   IOSTANDARD LVCMOS33 } [get_ports { sd_d[1] }]; #IO_L4P_T0_D04_14 Sch=sd_d[1]
set_property -dict { PACKAGE_PIN T20   IOSTANDARD LVCMOS33 } [get_ports { sd_d[2] }]; #IO_L6N_T0_D08_VREF_14 Sch=sd_d[2]
set_property -dict { PACKAGE_PIN U18   IOSTANDARD LVCMOS33 } [get_ports { sd_d[3] }]; #IO_L18N_T2_A11_D27_14 Sch=sd_d[3]
set_property -dict { PACKAGE_PIN V20   IOSTANDARD LVCMOS33 } [get_ports { sd_reset }]; #IO_L11N_T1_SRCC_14 Sch=sd_reset

## HDMI out
set_property -dict { PACKAGE_PIN U1    IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_clk_n }]; #IO_L1N_T0_34 Sch=hdmi_tx_clk_n
set_property -dict { PACKAGE_PIN T1    IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_clk_p }]; #IO_L1P_T0_34 Sch=hdmi_tx_clk_p
set_property -dict { PACKAGE_PIN Y1    IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_n[0] }]; #IO_L5N_T0_34 Sch=hdmi_tx_n[0]
set_property -dict { PACKAGE_PIN W1    IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_p[0] }]; #IO_L5P_T0_34 Sch=hdmi_tx_p[0]
set_property -dict { PACKAGE_PIN AB1   IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_n[1] }]; #IO_L7N_T1_34 Sch=hdmi_tx_n[1]
set_property -dict { PACKAGE_PIN AA1   IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_p[1] }]; #IO_L7P_T1_34 Sch=hdmi_tx_p[1]
set_property -dict { PACKAGE_PIN AB2   IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_n[2] }]; #IO_L8N_T1_34 Sch=hdmi_tx_n[2]
set_property -dict { PACKAGE_PIN AB3   IOSTANDARD TMDS_33  } [get_ports { hdmi_tx_p[2] }]; #IO_L8P_T1_34 Sch=hdmi_tx_p[2]

## Configuration options, can be used for all designs
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property CFGBVS VCCO [current_design]

## Design constraints

# Asynchronous inputs enter through two-flop synchronizers; the outputs
# go to a person and a UART, neither timed against our clocks.
set_false_path -from [get_ports { cpu_resetn uart_tx_in }]
set_false_path -to [get_ports { led[*] uart_rx_out }]

# SD MISO answers an SCK edge launched off the CPU clock, and spi.sv
# samples it two CPU cycles later. It arrives at most 25 ns after the
# launching edge: clock-to-out, board flight both ways, and the card's
# 14 ns output delay.
set sd_clk [get_clocks -of_objects [get_pins u_clk_rst/u_mmcm/CLKOUT0]]
set_input_delay -clock $sd_clk -max 25.0 [get_ports { sd_d[0] }]
set_input_delay -clock $sd_clk -min 0.0 [get_ports { sd_d[0] }]
set_multicycle_path -setup 2 -from [get_ports { sd_d[0] }]
set_multicycle_path -hold 1 -from [get_ports { sd_d[0] }]

# The card samples its inputs against SCK; its power switch is static.
set_false_path -to [get_ports { sd_cclk sd_cmd sd_d[*] sd_reset }]

# The CPU and DDR3 controller clocks are unrelated, and cross only
# through synchronizers and the memory stack's CDC FIFO. One period of
# the faster clock keeps a FIFO slot settled before the pointer that
# announces it clears its synchronizer, and the pointer's bits within a
# cycle of each other.
set cpu_clk  [get_clocks -of_objects [get_pins u_clk_rst/u_mmcm/CLKOUT0]]
set ctrl_clk [get_clocks -of_objects [get_pins u_ddr3_clk/u_pll/CLKOUT0]]
set cross_ns [expr {min([get_property PERIOD $cpu_clk], [get_property PERIOD $ctrl_clk])}]
set_max_delay -datapath_only -from $cpu_clk -to $ctrl_clk $cross_ns
set_max_delay -datapath_only -from $ctrl_clk -to $cpu_clk $cross_ns

# The HDMI pairs are source-synchronous: the monitor recovers its bit
# clock from the clock pair, which leaves through the same I/O-cell
# register and buffer as the data, so no output delay relates them to
# a clock of ours.
set_false_path -to [get_ports { hdmi_tx_p[*] hdmi_tx_n[*] hdmi_tx_clk_p hdmi_tx_clk_n }]

# The display crosses from the CPU clock into its pixel clock only
# through synchronizers (its control state and the output chain's
# reset) and inside dual-clock block RAM (its cells, palette and
# framebuffer), which has no timing arc between its ports. Both clocks
# come off the oscillator, so without this Vivado times the crossing
# at the closest pair of edges. One period of the faster clock bounds
# each bit's arrival, as on the DDR3 crossing. The pixel and serial
# clocks share an MMCM and stay timed as related clocks.
set pixel_clk [get_clocks -of_objects [get_pins u_video_clk/u_mmcm/CLKOUT1]]
set video_cross_ns [expr {min([get_property PERIOD $cpu_clk], [get_property PERIOD $pixel_clk])}]
set_max_delay -datapath_only -from $cpu_clk -to $pixel_clk $video_cross_ns
