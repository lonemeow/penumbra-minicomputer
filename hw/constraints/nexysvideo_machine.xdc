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

## Audio Codec
set_property -dict { PACKAGE_PIN T5    IOSTANDARD LVCMOS33 } [get_ports { ac_bclk }]; #IO_L14P_T2_SRCC_34 Sch=ac_bclk
set_property -dict { PACKAGE_PIN W6    IOSTANDARD LVCMOS33 } [get_ports { ac_dac_sdata }]; #IO_L15P_T2_DQS_34 Sch=ac_dac_sdata
set_property -dict { PACKAGE_PIN U5    IOSTANDARD LVCMOS33 } [get_ports { ac_lrclk }]; #IO_L14N_T2_SRCC_34 Sch=ac_lrclk
set_property -dict { PACKAGE_PIN U6    IOSTANDARD LVCMOS33 } [get_ports { ac_mclk }]; #IO_L16P_T2_34 Sch=ac_mclk

## IIC
set_property -dict { PACKAGE_PIN W5    IOSTANDARD LVCMOS33 } [get_ports { scl }]; #IO_L15N_T2_DQS_34 Sch=scl
set_property -dict { PACKAGE_PIN V5    IOSTANDARD LVCMOS33 } [get_ports { sda }]; #IO_L16N_T2_34 Sch=sda

## HID port
set_property -dict { PACKAGE_PIN W17   IOSTANDARD LVCMOS33   PULLUP true } [get_ports { ps2_clk }]; #IO_L16N_T2_A15_D31_14 Sch=ps2_clk
set_property -dict { PACKAGE_PIN N13   IOSTANDARD LVCMOS33   PULLUP true } [get_ports { ps2_data }]; #IO_L23P_T3_A03_D19_14 Sch=ps2_data

## Ethernet PHY
set_property -dict { PACKAGE_PIN AA16  IOSTANDARD LVCMOS25 } [get_ports { eth_mdc }]; #IO_L1N_T0_13 Sch=eth_mdc
set_property -dict { PACKAGE_PIN Y16   IOSTANDARD LVCMOS25 } [get_ports { eth_mdio }]; #IO_L1P_T0_13 Sch=eth_mdio
set_property -dict { PACKAGE_PIN U7    IOSTANDARD LVCMOS33 } [get_ports { eth_rst_b }]; #IO_25_34 Sch=eth_rst_b
set_property -dict { PACKAGE_PIN V13   IOSTANDARD LVCMOS25 } [get_ports { eth_rxck }]; #IO_L13P_T2_MRCC_13 Sch=eth_rxck
set_property -dict { PACKAGE_PIN W10   IOSTANDARD LVCMOS25 } [get_ports { eth_rxctl }]; #IO_L10N_T1_13 Sch=eth_rxctl
set_property -dict { PACKAGE_PIN AB16  IOSTANDARD LVCMOS25 } [get_ports { eth_rxd[0] }]; #IO_L2P_T0_13 Sch=eth_rxd[0]
set_property -dict { PACKAGE_PIN AA15  IOSTANDARD LVCMOS25 } [get_ports { eth_rxd[1] }]; #IO_L4P_T0_13 Sch=eth_rxd[1]
set_property -dict { PACKAGE_PIN AB15  IOSTANDARD LVCMOS25 } [get_ports { eth_rxd[2] }]; #IO_L4N_T0_13 Sch=eth_rxd[2]
set_property -dict { PACKAGE_PIN AB11  IOSTANDARD LVCMOS25 } [get_ports { eth_rxd[3] }]; #IO_L7P_T1_13 Sch=eth_rxd[3]
set_property -dict { PACKAGE_PIN AA14  IOSTANDARD LVCMOS25 } [get_ports { eth_txck }]; #IO_L5N_T0_13 Sch=eth_txck
set_property -dict { PACKAGE_PIN V10   IOSTANDARD LVCMOS25 } [get_ports { eth_txctl }]; #IO_L10P_T1_13 Sch=eth_txctl
set_property -dict { PACKAGE_PIN Y12   IOSTANDARD LVCMOS25 } [get_ports { eth_txd[0] }]; #IO_L11N_T1_SRCC_13 Sch=eth_txd[0]
set_property -dict { PACKAGE_PIN W12   IOSTANDARD LVCMOS25 } [get_ports { eth_txd[1] }]; #IO_L12N_T1_MRCC_13 Sch=eth_txd[1]
set_property -dict { PACKAGE_PIN W11   IOSTANDARD LVCMOS25 } [get_ports { eth_txd[2] }]; #IO_L12P_T1_MRCC_13 Sch=eth_txd[2]
set_property -dict { PACKAGE_PIN Y11   IOSTANDARD LVCMOS25 } [get_ports { eth_txd[3] }]; #IO_L11P_T1_SRCC_13 Sch=eth_txd[3]

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

# The codec drives BCLK and LRCLK only once its setup has told it to;
# until then both are inputs at either end, and the pull-downs keep them
# from floating. Both enter through two-flop synchronizers, and the
# output stage answers a BCLK edge within three CPU clocks, where half a
# BCLK is nearly ten: DAC_SDATA is timed against the codec's clock by
# that count, never by a path delay.
set_property PULLDOWN true [get_ports { ac_bclk ac_lrclk }]
set_false_path -from [get_ports { ac_bclk ac_lrclk }]
set_false_path -to [get_ports { ac_dac_sdata }]

# MCLK is a clock to the codec alone: the oscillator divided, with
# nothing in the FPGA timed against the pin.
set_false_path -to [get_ports { ac_mclk }]

# I2C runs at 100 kHz off the CPU clock, each line held for 150 CPU
# clocks at a time and read back through two-flop synchronizers.
set_false_path -from [get_ports { scl sda }]
set_false_path -to [get_ports { scl sda }]

# The PS/2 lines move tens of microseconds apart, on the attached
# device's clock, and are read back through two-flop synchronizers.
set_false_path -from [get_ports { ps2_clk ps2_data }]
set_false_path -to [get_ports { ps2_clk ps2_data }]

# The Ethernet PHY's receive clock. With software managing the PHY it
# is 25 or 2.5 MHz, for 100 or 10 Mbit/s. Until then the PHY negotiates
# by itself, and on a gigabit link the clock is 125 MHz. The device
# carries nothing at that speed, but its receive side is clocked at it,
# so everything on the clock is timed for it too. The periods are the
# shortest the PHY's datasheet allows each (Table 65), and the two never
# run together.
create_clock -name eth_rxck -period 36.000 [get_ports eth_rxck]
create_clock -name eth_rxck_1000 -period 7.200 -add [get_ports eth_rxck]
set_clock_groups -physically_exclusive -group eth_rxck -group eth_rxck_1000

# RXD and RX_CTL change within 0.5 ns of each edge of the clock the PHY
# sends (Table 65, TskewT), and their input cells take them on that same
# edge: the clock reaches the cells later than the data does, through
# its global buffer, and nothing delays the data. The clock as the PHY
# sends it is a clock of its own here, so that the paths from the pins
# can be told from the paths inside the receive domain. Each edge's data
# is checked against the capture on that edge, where Vivado would
# otherwise check it against the edge after. The pins take no pull:
# they are the PHY's configuration pins as it leaves reset.
create_clock -name eth_phy_rxc -period 36.000
set eth_rx_pins [get_ports { eth_rxd[*] eth_rxctl }]
set_input_delay -clock eth_phy_rxc -max 0.5 $eth_rx_pins
set_input_delay -clock eth_phy_rxc -min -0.5 $eth_rx_pins
set_input_delay -clock eth_phy_rxc -clock_fall -max 0.5 -add_delay $eth_rx_pins
set_input_delay -clock eth_phy_rxc -clock_fall -min -0.5 -add_delay $eth_rx_pins
set_multicycle_path -setup 0 -rise_from [get_clocks eth_phy_rxc] -rise_to [get_clocks eth_rxck]
set_multicycle_path -setup 0 -fall_from [get_clocks eth_phy_rxc] -fall_to [get_clocks eth_rxck]
# At 125 MHz the pins carry nothing the device takes.
set_false_path -from [get_clocks eth_phy_rxc] -to [get_clocks eth_rxck_1000]

# The transmit pins are source-synchronous: the PHY samples TXD and
# TX_CTL against the clock pin, which leaves through the same kind of
# I/O-cell register as they do, on the same clock. They change a quarter
# of the pin clock's period ahead of its rise by count, never by a path
# delay.
set_false_path -to [get_ports { eth_txck eth_txctl eth_txd[*] }]

# The Ethernet device's receive side runs on the PHY's clock and its
# transmit side on the oscillator. Each meets the CPU clock only through
# eth_cdc, in synchronizers and bundles held still while they cross, and
# inside dual-clock block RAM. The PHY's clock is unrelated to the CPU
# clock; the oscillator is the CPU clock's own source, so without this
# Vivado times that crossing at the closest pair of edges. One period of
# the faster clock bounds each bit's arrival, as on the other crossings.
set osc_clk [get_clocks sys_clk_pin]
set eth_rx_clks [get_clocks { eth_rxck eth_rxck_1000 }]
set_max_delay -datapath_only -from $cpu_clk -to $eth_rx_clks 7.200
set_max_delay -datapath_only -from $eth_rx_clks -to $cpu_clk 7.200
set_max_delay -datapath_only -from $cpu_clk -to $osc_clk 10.000
set_max_delay -datapath_only -from $osc_clk -to $cpu_clk 10.000

# PHY management runs at 2.5 MHz at most off the CPU clock: MDC from a
# divider, MDIO changed half an MDC period from the edge the PHY samples
# on and read back through a two-flop synchronizer. The PHY's reset pin
# moves once per board reset.
set_false_path -from [get_ports { eth_mdio }]
set_false_path -to [get_ports { eth_mdc eth_mdio eth_rst_b }]
