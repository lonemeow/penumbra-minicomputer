// Nexys Video board top (Penumbra/4): the gen4 machine on the XC7A200T
//
// A fork of nexysvideo_penumbra3_top that differs only in the machine: the
// gen4 machine keeps the gen2 one's port list, so the board attaches to it
// the same way.
//
// Wires machine_penumbra4 to the board: the shared clocking and power-on
// reset (nexysvideo_clk_rst), a 64 KB boot ROM, the UART on the UART USB
// port, the microSD slot over SPI, the display adapter on the HDMI
// output, the audio device in front of the board's codec, the Ethernet
// device on the board's PHY and the PS/2 port on the board's HID socket
// behind autoconfig, and system RAM in the board's 512 MiB of DDR3. The top
// grows one device per bring-up step; the external
// Penumbra Bus is assembled here the way the ULX3S top assembles it
// (device decode, response fabric with its no-device watchdog), so what
// simulates is what synthesizes.
//
// The display runs its output chain on clocks of its own
// (nexysvideo_video_clk), and resets with the machine or while they are
// unlocked. Its four lanes leave through a DDR register in each pair's
// I/O cell into a TMDS_33 differential buffer.
//
// The audio device plays through the board's ADAU1761 codec
// (doc/internals/nexysvideo/audio.md). The codec drives the serial
// link's clocks, so the device's output side runs on the CPU clock and
// follows them from there. Its registers are set over I2C after every
// reset of the machine, which the device reports as its STATUS, and its
// master clock is the oscillator divided, running whatever the machine's
// reset does.
//
// The Ethernet device is on the board's RTL8211E PHY (nexysvideo_eth,
// doc/internals/nexysvideo/ethernet.md). The PHY is reset with the
// board, and the machine is held in reset until the PHY answers
// management frames. The device's factory address is read from the
// board's EEPROM, on the I2C bus the codec is on.
//
// The DDR3 runs on clocks of its own (nexysvideo_ddr3_clk) behind the
// line stack (ddr3_stack), which crosses to them. Its controller resets
// only while those clocks are unlocked: CPU RESET restarts the machine
// and the stack, never the controller, so it never recalibrates. The
// controller stalls every access until it has calibrated, longer than
// the fabric's watchdog waits, so the machine is held in reset until
// then.
//
// The LEDs report the board-level pieces under the machine: led[0] MMCM
// locked, led[1] reset released, led[2] a 1 Hz heartbeat counted off the
// CPU clock, led[3] the DDR3 calibrated, led[7] the die above the
// thermal guard's warning threshold. led[6:4] read the longest a RAM
// access has waited since configuration against the fabric's watchdog:
// led[4] at least an eighth of its limit, led[5] a quarter, led[6] a
// half. The console is 115200 8N1.

// Channels the audio device is built with, set by the build as on the
// ULX3S.
`ifndef AUDIO_CHANNELS
`define AUDIO_CHANNELS 1
`endif

module nexysvideo_penumbra4_top (
    input  logic       clk,          // 100 MHz oscillator
    input  logic       cpu_resetn,   // CPU RESET button, low while pressed
    output logic [7:0] led,
    output logic       uart_rx_out,  // FPGA TX, to the host's RX
    input  logic       uart_tx_in,   // FPGA RX, from the host's TX

    // ── microSD slot, in SPI mode ──────────────────────────
    output logic       sd_cclk,      // SCK
    output logic       sd_cmd,       // MOSI
    inout  wire  [3:0] sd_d,         // [0] MISO in, [3] CS out, [2:1] held high
    output logic       sd_reset,     // card power: low powers the slot

    // ── HDMI output (TX), DVI signalling ───────────────────
    output logic [2:0] hdmi_tx_p,    // TMDS lanes: [0] blue, [1] green, [2] red
    output logic [2:0] hdmi_tx_n,
    output logic       hdmi_tx_clk_p,
    output logic       hdmi_tx_clk_n,

    // ── Audio codec (ADAU1761) ─────────────────────────────
    output logic       ac_mclk,      // the codec's master clock, 12.5 MHz
    input  logic       ac_bclk,      // the serial link's bit clock, the codec's
    input  logic       ac_lrclk,     // and its frame clock, the codec's too
    output logic       ac_dac_sdata, // samples to the codec's DACs

    // ── I2C: the codec, and the MAC-address EEPROM ─────────
    inout  wire        scl,
    inout  wire        sda,

    // ── Ethernet PHY (RTL8211E): RGMII, management, reset ──
    input  logic       eth_rxck,     // the PHY's receive clock
    input  logic       eth_rxctl,
    input  logic [3:0] eth_rxd,
    output logic       eth_txck,
    output logic       eth_txctl,
    output logic [3:0] eth_txd,
    output logic       eth_mdc,
    inout  wire        eth_mdio,
    output logic       eth_rst_b,    // low resets the PHY

    // ── USB HID socket, through the board's PIC24: a PS/2 link ──
    inout  wire        ps2_clk,
    inout  wire        ps2_data,

    // ── DDR3 (MT41K256M16, x16) ───────────────────────────
    output logic [0:0]  ddr3_ck_p,
    output logic [0:0]  ddr3_ck_n,
    output logic        ddr3_reset_n,
    output logic [0:0]  ddr3_cke,
    output logic        ddr3_ras_n,
    output logic        ddr3_cas_n,
    output logic        ddr3_we_n,
    output logic [14:0] ddr3_addr,
    output logic [2:0]  ddr3_ba,
    inout  wire  [15:0] ddr3_dq,
    inout  wire  [1:0]  ddr3_dqs_p,
    inout  wire  [1:0]  ddr3_dqs_n,
    output logic [1:0]  ddr3_dm,
    output logic [0:0]  ddr3_odt
);
    import penumbra_pkg::*;

    localparam longint CPU_HZ    = 80_000_000;
    localparam int     RAM_BYTES = 512 * 1024 * 1024;
    localparam int     ROM_WORDS = 16384;       // 64 KiB

    // The memory port's shape, as nexysvideo_ddr3 declares it: 16-byte
    // bursts over 512 MiB, and the tag each request carries.
    localparam int DDR3_ADDR_BITS = 25;
    localparam int DDR3_AUX_WIDTH = 4;

    // The fabric's watchdog limit, which the RAM wait LEDs read against.
    localparam int BUS_TIMEOUT_CYCLES = 256;
    localparam int WAIT_W             = $clog2(BUS_TIMEOUT_CYCLES);

    logic cpu_clk;
    logic locked;
    logic board_rst;

    nexysvideo_clk_rst #(
        .CPU_HZ (CPU_HZ)
    ) u_clk_rst (
        .i_clk_100mhz (clk),
        .i_cpu_resetn (cpu_resetn),
        .o_clk        (cpu_clk),
        .o_locked     (locked),
        .o_rst        (board_rst)
    );

    // ── DDR3 clocks and resets ──────────────────────────────
    logic ctrl_clk, ddr3_clk, ddr3_clk_90, ref_clk;
    logic ddr3_locked;

    nexysvideo_ddr3_clk u_ddr3_clk (
        .i_clk_100mhz  (clk),
        .o_ctrl_clk    (ctrl_clk),
        .o_ddr3_clk    (ddr3_clk),
        .o_ddr3_clk_90 (ddr3_clk_90),
        .o_ref_clk     (ref_clk),
        .o_locked      (ddr3_locked)
    );

    // The controller's reset: its clocks' lock alone, asynchronous to the
    // controller clock, through two flops.
    (* ASYNC_REG = "TRUE" *) logic ddr3_locked_sync1, ddr3_locked_sync2;

    always_ff @(posedge ctrl_clk) begin
        ddr3_locked_sync1 <= ddr3_locked;
        ddr3_locked_sync2 <= ddr3_locked_sync1;
    end

    // The machine's reset: the board's, held on until the memory has
    // calibrated (calib_complete, in the controller clock's domain) and
    // the Ethernet PHY has come out of its own reset (phy_ready, on the
    // CPU clock).
    logic calib_complete;
    (* ASYNC_REG = "TRUE" *) logic calib_sync1, calib_sync2;

    always_ff @(posedge cpu_clk) begin
        calib_sync1 <= calib_complete;
        calib_sync2 <= calib_sync1;
    end

    logic phy_ready;

    logic rst;
    assign rst = board_rst || !calib_sync2 || !phy_ready;

    // The stack's controller side, and the display's output chain below,
    // reset with the machine. The reset crosses from a flop, since a
    // synchronizer sampling logic can catch a glitch the logic's inputs
    // never held.
    logic rst_q;
    (* ASYNC_REG = "TRUE" *) logic stack_rst_sync1, stack_rst_sync2;

    always_ff @(posedge cpu_clk)
        rst_q <= rst;

    always_ff @(posedge ctrl_clk) begin
        stack_rst_sync1 <= rst_q;
        stack_rst_sync2 <= stack_rst_sync1;
    end

    // ── Display clocks and reset ────────────────────────────
    logic pixel_clk, serial_clk;
    logic video_locked;

    nexysvideo_video_clk u_video_clk (
        .i_clk_100mhz (clk),
        .o_pixel_clk  (pixel_clk),
        .o_serial_clk (serial_clk),
        .o_locked     (video_locked)
    );

    // The output chain's reset, in the pixel domain: the machine's, or
    // the chain's clocks out of lock. Each reaches the domain through its
    // own two flops, and they combine after, into the one flop that
    // drives both of the chain's domains; the chain absorbs the release's
    // skew between them by design (video_serializer).
    (* ASYNC_REG = "TRUE" *) logic video_rst_sync1, video_rst_sync2;
    (* ASYNC_REG = "TRUE" *) logic video_locked_sync1, video_locked_sync2;
    logic video_rst_q;

    always_ff @(posedge pixel_clk) begin
        video_rst_sync1    <= rst_q;
        video_rst_sync2    <= video_rst_sync1;
        video_locked_sync1 <= video_locked;
        video_locked_sync2 <= video_locked_sync1;
        video_rst_q        <= video_rst_sync2 || !video_locked_sync2;
    end

    // Over-temperature shutdown, off the oscillator itself.
    logic thermal_warn;

    xc7_thermal_guard u_thermal (
        .i_clk  (clk),
        .o_warn (thermal_warn)
    );

    // ══════════════════════════════════════════════════════════
    // External Penumbra Bus — driven by the machine, decoded here
    // ══════════════════════════════════════════════════════════
    logic [31:0] mem_addr, mem_wdata, mem_rdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic        mem_ack;
    logic        bus_fault;

    logic        uart_irq;
    logic        spi_irq;
    logic        audio_irq;
    logic        eth_irq;
    logic        ps2_irq;

    // ── Autoconfig control from the machine's bus controller ──
    logic        busctl_bus_rst;
    logic        busctl_cfg_en;

    machine_penumbra4 #(
        .MACH_NAME0 (32'h7978654E),   // "Nexy"
        .MACH_NAME1 (32'h69562073),   // "s Vi"
        .MACH_NAME2 (32'h006F6564),   // "deo\0"
        .CPU_FREQ   (32'(CPU_HZ))
    ) u_machine (
        .i_clk         (cpu_clk),
        .i_rst         (rst),

        .i_irq         (uart_irq | spi_irq | audio_irq | eth_irq | ps2_irq),
        .i_timer_irq   (1'b0),        // the internal timer is the source

        .o_bus_addr    (mem_addr),
        .o_bus_wdata   (mem_wdata),
        .o_bus_byte_en (mem_byte_en),
        .o_bus_re      (mem_re),
        .o_bus_we      (mem_we),
        .i_bus_rdata   (mem_rdata),
        .i_bus_ack     (mem_ack),
        .i_bus_fault   (bus_fault),

        .o_bus_rst     (busctl_bus_rst),
        .o_bus_cfg_en  (busctl_cfg_en),

        // Commit observability, unused on the board.
        /* verilator lint_off PINCONNECTEMPTY */
        .o_landing_valid    (),
        .o_landing_pc       (),
        .o_landing_op_class (),
        .o_landing_trap     (),
        .o_landing_vector   (),
        .o_landing_older    (),
        .o_sr               (),
        .o_commit_we        (),
        .o_commit_phys      (),
        .o_commit_data      (),
        .o_retire_count     (),
        .o_prog_end         ()
        /* verilator lint_on PINCONNECTEMPTY */
    );

    // ── System RAM, in DDR3 ─────────────────────────────────
    logic        ram_sel;
    logic [31:0] ram_rdata;
    logic        ram_ack;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_BYTES)))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));

    logic                      wb_stb, wb_we, wb_stall, wb_ack;
    logic [DDR3_ADDR_BITS-1:0] wb_addr;
    logic [127:0]              wb_wdata, wb_rdata;
    logic [15:0]               wb_sel;
    logic [DDR3_AUX_WIDTH-1:0] wb_aux_out, wb_aux_in;

    ddr3_stack #(
        .ADDR_BITS  (DDR3_ADDR_BITS),
        .AUX_WIDTH  (DDR3_AUX_WIDTH)
    ) u_ram (
        .i_sys_clk  (cpu_clk),
        .i_sys_rst  (rst),
        .i_ctrl_clk (ctrl_clk),
        .i_ctrl_rst (stack_rst_sync2),
        .i_addr     (mem_addr),
        .i_wdata    (mem_wdata),
        .i_byte_en  (mem_byte_en),
        .i_we       (mem_we & ram_sel),
        .i_re       (mem_re & ram_sel),
        .o_rdata    (ram_rdata),
        .o_ack      (ram_ack),
        .o_wb_stb   (wb_stb),
        .o_wb_we    (wb_we),
        .o_wb_addr  (wb_addr),
        .o_wb_data  (wb_wdata),
        .o_wb_sel   (wb_sel),
        .o_wb_aux   (wb_aux_out),
        .i_wb_stall (wb_stall),
        .i_wb_ack   (wb_ack),
        .i_wb_data  (wb_rdata),
        .i_wb_aux   (wb_aux_in)
    );

    /* verilator lint_off PINCONNECTEMPTY */
    nexysvideo_ddr3 #(
        .BIST_MODE (0)
    ) u_ddr3 (
        .i_ctrl_clk       (ctrl_clk),
        .i_ddr3_clk       (ddr3_clk),
        .i_ddr3_clk_90    (ddr3_clk_90),
        .i_ref_clk        (ref_clk),
        .i_rst_n          (ddr3_locked_sync2),

        .i_stb            (wb_stb),
        .i_we             (wb_we),
        .i_addr           (wb_addr),
        .i_data           (wb_wdata),
        .i_sel            (wb_sel),
        .i_aux            (wb_aux_out),
        .o_stall          (wb_stall),
        .o_ack            (wb_ack),
        .o_data           (wb_rdata),
        .o_aux            (wb_aux_in),

        .i_dbg_stb        (1'b0),
        .i_dbg_we         (1'b0),
        .i_dbg_addr       ('0),
        .i_dbg_data       (32'h0),
        .o_dbg_stall      (),
        .o_dbg_ack        (),
        .o_dbg_data       (),

        .o_calib_complete (calib_complete),
        .o_calib_state    (),

        .o_ddr3_ck_p      (ddr3_ck_p[0]),
        .o_ddr3_ck_n      (ddr3_ck_n[0]),
        .o_ddr3_reset_n   (ddr3_reset_n),
        .o_ddr3_cke       (ddr3_cke[0]),
        .o_ddr3_ras_n     (ddr3_ras_n),
        .o_ddr3_cas_n     (ddr3_cas_n),
        .o_ddr3_we_n      (ddr3_we_n),
        .o_ddr3_addr      (ddr3_addr),
        .o_ddr3_ba        (ddr3_ba),
        .io_ddr3_dq       (ddr3_dq),
        .io_ddr3_dqs_p    (ddr3_dqs_p),
        .io_ddr3_dqs_n    (ddr3_dqs_n),
        .o_ddr3_dm        (ddr3_dm),
        .o_ddr3_odt       (ddr3_odt[0])
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // The longest a RAM access has waited, for the LEDs.
    logic [WAIT_W-1:0] ram_wait_max;

    bus_wait_meter #(
        .WIDTH (WAIT_W)
    ) u_ram_wait (
        .i_clk (cpu_clk),
        .i_req ((mem_re | mem_we) & ram_sel),
        .i_ack (ram_ack),
        .o_max (ram_wait_max)
    );

    // ── Boot ROM ────────────────────────────────────────────
    logic        rom_sel;
    logic [31:0] rom_rdata;
    logic        rom_ack;

    bus_devsel #(.BASE(ROM_BASE), .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel (.i_addr(mem_addr), .o_sel(rom_sel));

    boot_rom #(
        .ROM_WORDS (ROM_WORDS)
    ) u_rom (
        .i_clk   (cpu_clk),
        .i_rst   (rst),
        .i_addr  (mem_addr),
        .i_re    (mem_re & rom_sel),
        .i_we    (mem_we & rom_sel),
        .o_rdata (rom_rdata),
        .o_ack   (rom_ack)
    );

    // ── UART ────────────────────────────────────────────────
    logic        uart_sel;
    logic [31:0] uart_rdata;
    logic        uart_ack;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'd4096))
        u_uart_sel (.i_addr(mem_addr), .o_sel(uart_sel));

    uart #(
        .CLK_FREQ (int'(CPU_HZ))
    ) u_uart (
        .i_clk   (cpu_clk),
        .i_rst   (rst),
        .i_addr  (mem_addr),
        .i_wdata (mem_wdata),
        .i_we    (mem_we & uart_sel),
        .i_re    (mem_re & uart_sel),
        .o_rdata (uart_rdata),
        .o_ack   (uart_ack),
        .o_tx    (uart_rx_out),
        .i_rx    (uart_tx_in),
        .o_irq   (uart_irq)
    );

    // ══════════════════════════════════════════════════════════
    // Autoconfig device chain
    //
    // busctl_cfg_en (from the machine) starts the daisy chain; each
    // device's cfg_out feeds the next cfg_in. A bus fault at chain end
    // signals "no more devices" to the ROM autoconfig loop.
    // ══════════════════════════════════════════════════════════

    // ── SD card over SPI (first in chain) ───────────────────
    logic [31:0] spi_dev_addr, spi_dev_wdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    // cfg_out is the daisy-chain to the next autoconfig device (display).
    logic        ac_spi_cfg_out;
    logic        spi_dev_we, spi_dev_re;
    logic [31:0] spi_dev_rdata;
    logic        spi_dev_ack;

    logic [31:0] ac_spi_rdata;
    logic        ac_spi_ack, ac_spi_sel;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_SD),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00004453)      // "SD\0\0" packed LE
    ) u_ac_spi (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .i_bus_rst    (busctl_bus_rst),
        .i_cfg_en     (busctl_cfg_en),
        .i_cfg_in     (busctl_cfg_en), // first in chain
        .o_cfg_out    (ac_spi_cfg_out),
        .i_addr       (mem_addr),
        .i_wdata      (mem_wdata),
        .i_byte_en    (mem_byte_en),
        .i_we         (mem_we),
        .i_re         (mem_re),
        .o_rdata      (ac_spi_rdata),
        .o_ack        (ac_spi_ack),
        .o_sel        (ac_spi_sel),
        .o_dev_addr   (spi_dev_addr),
        .o_dev_wdata  (spi_dev_wdata),
        .o_dev_byte_en(spi_dev_byte_en),
        .o_dev_we     (spi_dev_we),
        .o_dev_re     (spi_dev_re),
        .i_dev_rdata  (spi_dev_rdata),
        .i_dev_ack    (spi_dev_ack)
    );

    logic spi_cs0;

    spi #(
        .CLK_FREQ   (int'(CPU_HZ)),
        .FIFO_DEPTH (512)
    ) u_spi (
        .i_clk   (cpu_clk),
        .i_rst   (rst),
        .i_addr  (spi_dev_addr),
        .i_wdata (spi_dev_wdata),
        .i_we    (spi_dev_we),
        .i_re    (spi_dev_re),
        .o_rdata (spi_dev_rdata),
        .o_ack   (spi_dev_ack),
        .o_sclk  (sd_cclk),
        .o_mosi  (sd_cmd),
        .i_miso  (sd_d[0]),
        .o_cs0   (spi_cs0),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_cs1   (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_irq   (spi_irq)
    );

    // SPI mode on the SD pins: DAT0 is the card's MISO, DAT3 its chip
    // select, and DAT1/DAT2 are unused and held high.
    assign sd_d[0] = 1'bz;
    assign sd_d[3] = spi_cs0;
    assign sd_d[2] = 1'b1;
    assign sd_d[1] = 1'b1;

    // The slot is unpowered until the FPGA drives SD_RESET low, and it
    // stays off while the machine is held in reset: every CPU RESET
    // power-cycles the card, which returns a card left mid-transaction
    // to its power-on state, the one reset a card cannot be denied.
    assign sd_reset = rst;

    // ── Display adapter (second in chain) ───────────────────
    // The character console and framebuffer on the HDMI output:
    // video_display behind the autoconfig wrapper, as on the ULX3S. No
    // cell preload and CTRL.ENABLE resets clear, so the screen stays
    // black until the boot ROM draws it — a reset never shows stale
    // cell memory.
    logic [31:0] disp_dev_addr, disp_dev_wdata;
    // The pixel aperture commits individual lanes, so the display is
    // the one autoconfig device here that consumes byte enables.
    logic [3:0]  disp_dev_byte_en;
    logic        disp_dev_we, disp_dev_re;
    logic [31:0] disp_dev_rdata;
    logic        disp_dev_ack;

    logic [31:0] ac_disp_rdata;
    logic        ac_disp_ack, ac_disp_sel;
    // cfg_out is the daisy-chain to the next autoconfig device (audio).
    logic        ac_disp_cfg_out;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_DISPLAY),
        .DEV_SIZE  (32'd262144),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h50534944),     // "DISP" packed LE
        .DEV_NAME1 (32'h0059414C)      // "LAY\0"
    ) u_ac_disp (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .i_bus_rst    (busctl_bus_rst),
        .i_cfg_en     (busctl_cfg_en),
        .i_cfg_in     (ac_spi_cfg_out),
        .o_cfg_out    (ac_disp_cfg_out),
        .i_addr       (mem_addr),
        .i_wdata      (mem_wdata),
        .i_byte_en    (mem_byte_en),
        .i_we         (mem_we),
        .i_re         (mem_re),
        .o_rdata      (ac_disp_rdata),
        .o_ack        (ac_disp_ack),
        .o_sel        (ac_disp_sel),
        .o_dev_addr   (disp_dev_addr),
        .o_dev_wdata  (disp_dev_wdata),
        .o_dev_byte_en(disp_dev_byte_en),
        .o_dev_we     (disp_dev_we),
        .o_dev_re     (disp_dev_re),
        .i_dev_rdata  (disp_dev_rdata),
        .i_dev_ack    (disp_dev_ack)
    );

    // The DDR bit pairs per lane, in the display's lane order: [0] blue,
    // [1] green, [2] red, [3] the clock. Bit 2n of a TMDS word rides
    // hdmi_lane_d0 and bit 2n+1 hdmi_lane_d1, both changing on the
    // serial clock's rising edge (video_serializer).
    logic [3:0] hdmi_lane_d0, hdmi_lane_d1;

    video_display u_display (
        .i_clk     (cpu_clk),
        .i_rst     (rst),
        .i_addr    (disp_dev_addr),
        .i_wdata   (disp_dev_wdata),
        .i_byte_en (disp_dev_byte_en),
        .i_we      (disp_dev_we),
        .i_re      (disp_dev_re),
        .o_rdata   (disp_dev_rdata),
        .o_ack     (disp_dev_ack),
        .i_pclk    (pixel_clk),
        .i_sclk    (serial_clk),
        .i_vrst    (video_rst_q),
        .o_d0      (hdmi_lane_d0),
        .o_d1      (hdmi_lane_d1)
    );

    // HDMI pads: per lane, an ODDR, the bit stream's last register, in
    // the pair's I/O cell — no fabric routing on the bit stream — into
    // an OBUFDS, whose TMDS_33 standard the constraints set. SAME_EDGE
    // takes both halves of a pair on the rising edge that ends it, as
    // the ULX3S's ODDRX1F does; OPPOSITE_EDGE would sample D2 on the
    // falling edge, after the serializer has moved on to the next pair.
    logic [3:0] hdmi_pad_p, hdmi_pad_n;

    for (genvar i = 0; i < 4; i++) begin : g_hdmi
        logic serial_data;

        ODDR #(
            .DDR_CLK_EDGE ("SAME_EDGE")
        ) u_hdmi_oddr (
            .D1 (hdmi_lane_d0[i]),
            .D2 (hdmi_lane_d1[i]),
            .Q  (serial_data),
            .C  (serial_clk),
            .CE (1'b1),
            .R  (1'b0),
            .S  (1'b0)
        );

        OBUFDS u_hdmi_obufds (
            .I  (serial_data),
            .O  (hdmi_pad_p[i]),
            .OB (hdmi_pad_n[i])
        );
    end

    assign hdmi_tx_p     = hdmi_pad_p[2:0];
    assign hdmi_tx_n     = hdmi_pad_n[2:0];
    assign hdmi_tx_clk_p = hdmi_pad_p[3];
    assign hdmi_tx_clk_n = hdmi_pad_n[3];

    // ── Audio (third in chain) ──────────────────────────────
    // The audio device as the ULX3S builds it, its output side on the
    // CPU clock: the codec sets the sample rate, so no clock here has to
    // divide to it.
    logic [31:0] audio_dev_addr, audio_dev_wdata, audio_dev_rdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  audio_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        audio_dev_we, audio_dev_re, audio_dev_ack;
    logic [31:0] ac_audio_rdata;
    logic        ac_audio_ack, ac_audio_sel;
    // cfg_out is the daisy-chain to the next autoconfig device
    // (Ethernet).
    logic        ac_audio_cfg_out;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_AUDIO),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h49445541),     // "AUDI" packed LE
        .DEV_NAME1 (32'h0000004F)      // "O\0\0\0"
    ) u_ac_audio (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .i_bus_rst    (busctl_bus_rst),
        .i_cfg_en     (busctl_cfg_en),
        .i_cfg_in     (ac_disp_cfg_out),
        .o_cfg_out    (ac_audio_cfg_out),
        .i_addr       (mem_addr),
        .i_wdata      (mem_wdata),
        .i_byte_en    (mem_byte_en),
        .i_we         (mem_we),
        .i_re         (mem_re),
        .o_rdata      (ac_audio_rdata),
        .o_ack        (ac_audio_ack),
        .o_sel        (ac_audio_sel),
        .o_dev_addr   (audio_dev_addr),
        .o_dev_wdata  (audio_dev_wdata),
        .o_dev_byte_en(audio_dev_byte_en),
        .o_dev_we     (audio_dev_we),
        .o_dev_re     (audio_dev_re),
        .i_dev_rdata  (audio_dev_rdata),
        .i_dev_ack    (audio_dev_ack)
    );

    localparam int AUDIO_SAMPLE_HZ = 48_000;
    // The serial link as the codec's setup configures it: 64 BCLKs a
    // frame.
    localparam int AUDIO_BCLK_HZ   = 64 * AUDIO_SAMPLE_HZ;
    localparam int CODEC_MCLK_HZ   = 12_500_000;

    logic signed [15:0] audio_frame_l, audio_frame_r;
    /* verilator lint_off UNUSEDSIGNAL */
    logic               audio_frame_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic               audio_frame_req;

    logic       codec_busy, codec_fault;
    logic [7:0] codec_fault_code;

    audio #(
        .CHANNELS  (`AUDIO_CHANNELS),
        .DEPTH     (1024),
        .SAMPLE_HZ (AUDIO_SAMPLE_HZ)
    ) u_audio (
        .i_clk            (cpu_clk),
        .i_rst            (rst),
        .i_addr           (audio_dev_addr),
        .i_wdata          (audio_dev_wdata),
        .i_we             (audio_dev_we),
        .i_re             (audio_dev_re),
        .o_rdata          (audio_dev_rdata),
        .o_ack            (audio_dev_ack),
        .o_irq            (audio_irq),
        .i_out_busy       (codec_busy),
        .i_out_fault      (codec_fault),
        .i_out_fault_code (codec_fault_code),
        .i_aclk           (cpu_clk),
        .i_arst           (rst),
        .i_frame_req      (audio_frame_req),
        .o_frame_l        (audio_frame_l),
        .o_frame_r        (audio_frame_r),
        .o_frame_en       (audio_frame_en)
    );

    audio_i2s_out #(
        .CLK_HZ  (int'(CPU_HZ)),
        .BCLK_HZ (AUDIO_BCLK_HZ)
    ) u_audio_out (
        .i_clk       (cpu_clk),
        .i_rst       (rst),
        .i_frame_l   (audio_frame_l),
        .i_frame_r   (audio_frame_r),
        .o_frame_req (audio_frame_req),
        .i_bclk      (ac_bclk),
        .i_lrclk     (ac_lrclk),
        .o_sdata     (ac_dac_sdata)
    );

    // ── The codec's master clock and its setup ──────────────
    // MCLK runs from configuration on, so the codec has had it for the
    // whole reset hold by the time the setup first addresses it.
    nexysvideo_codec_mclk #(
        .MCLK_HZ (CODEC_MCLK_HZ)
    ) u_codec_mclk (
        .i_clk_100mhz (clk),
        .o_mclk       (ac_mclk)
    );

    // ── The I2C bus ─────────────────────────────────────────
    // The codec and the EEPROM that holds the board's Ethernet address
    // are on the one bus. Its master has a command port for each of the
    // two that talk to them, the codec's setup and the Ethernet device,
    // which have it a transaction at a time.
    localparam int I2C_CODEC     = 0;
    localparam int I2C_EEPROM    = 1;
    localparam int I2C_CONSUMERS = 2;

    logic [I2C_CONSUMERS-1:0]      i2c_cmd_valid, i2c_cmd_ready;
    logic [I2C_CONSUMERS-1:0]      i2c_cmd_start, i2c_cmd_stop;
    logic [I2C_CONSUMERS-1:0]      i2c_cmd_read, i2c_cmd_nack;
    logic [I2C_CONSUMERS-1:0][7:0] i2c_cmd_wdata;
    logic [I2C_CONSUMERS-1:0]      i2c_rsp_valid;
    logic [7:0]                    i2c_rsp_rdata;
    logic                          i2c_rsp_nack, i2c_rsp_error;

    nexysvideo_codec_setup #(
        .CLK_HZ  (int'(CPU_HZ)),
        .MCLK_HZ (CODEC_MCLK_HZ)
    ) u_codec_setup (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .o_cmd_valid  (i2c_cmd_valid[I2C_CODEC]),
        .i_cmd_ready  (i2c_cmd_ready[I2C_CODEC]),
        .o_cmd_start  (i2c_cmd_start[I2C_CODEC]),
        .o_cmd_stop   (i2c_cmd_stop[I2C_CODEC]),
        .o_cmd_read   (i2c_cmd_read[I2C_CODEC]),
        .o_cmd_wdata  (i2c_cmd_wdata[I2C_CODEC]),
        .o_cmd_nack   (i2c_cmd_nack[I2C_CODEC]),
        .i_rsp_valid  (i2c_rsp_valid[I2C_CODEC]),
        .i_rsp_rdata  (i2c_rsp_rdata),
        .i_rsp_nack   (i2c_rsp_nack),
        .i_rsp_error  (i2c_rsp_error),
        .o_busy       (codec_busy),
        .o_fault      (codec_fault),
        .o_fault_code (codec_fault_code)
    );

    // What the master asks of each line: 1 lets it go, 0 pulls it low.
    logic i2c_scl_release, i2c_sda_release;

    i2c_shared_master #(
        .CLK_HZ    (int'(CPU_HZ)),
        .SCL_HZ    (100_000),
        .CONSUMERS (I2C_CONSUMERS)
    ) u_i2c (
        .i_clk       (cpu_clk),
        .i_rst       (rst),
        .i_cmd_valid (i2c_cmd_valid),
        .o_cmd_ready (i2c_cmd_ready),
        .i_cmd_start (i2c_cmd_start),
        .i_cmd_stop  (i2c_cmd_stop),
        .i_cmd_read  (i2c_cmd_read),
        .i_cmd_wdata (i2c_cmd_wdata),
        .i_cmd_nack  (i2c_cmd_nack),
        .o_rsp_valid (i2c_rsp_valid),
        .o_rsp_rdata (i2c_rsp_rdata),
        .o_rsp_nack  (i2c_rsp_nack),
        .o_rsp_error (i2c_rsp_error),
        .o_scl       (i2c_scl_release),
        .i_scl       (scl),
        .o_sda       (i2c_sda_release),
        .i_sda       (sda)
    );

    // I2C pads, open drain: the board's pull-ups make a line high
    assign sda = i2c_sda_release ? 1'bz : 1'b0;
    assign scl = i2c_scl_release ? 1'bz : 1'b0;

    // ── Ethernet (fourth in chain) ──────────────────────────
    // The device's rings, and its window, which ends where the receive
    // ring's slots do.
    localparam int          ETH_RX_SLOTS   = 32;
    localparam int          ETH_TX_SLOTS   = 8;
    localparam int          ETH_SLOT_BYTES = 2048;
    localparam logic [31:0] ETH_DEV_SIZE   = ETH_RX_APERTURE +
                                             ETH_RX_SLOTS * ETH_SLOT_BYTES;

    logic [31:0] eth_dev_addr, eth_dev_wdata, eth_dev_rdata;
    logic [3:0]  eth_dev_byte_en;
    logic        eth_dev_we, eth_dev_re, eth_dev_ack;
    logic [31:0] ac_eth_rdata;
    logic        ac_eth_ack, ac_eth_sel;
    // cfg_out is the daisy-chain to the next autoconfig device (PS/2 port).
    logic        ac_eth_cfg_out;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_ETHERNET),
        .DEV_SIZE  (ETH_DEV_SIZE),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00485445)      // "ETH\0" packed LE
    ) u_ac_eth (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .i_bus_rst    (busctl_bus_rst),
        .i_cfg_en     (busctl_cfg_en),
        .i_cfg_in     (ac_audio_cfg_out),
        .o_cfg_out    (ac_eth_cfg_out),
        .i_addr       (mem_addr),
        .i_wdata      (mem_wdata),
        .i_byte_en    (mem_byte_en),
        .i_we         (mem_we),
        .i_re         (mem_re),
        .o_rdata      (ac_eth_rdata),
        .o_ack        (ac_eth_ack),
        .o_sel        (ac_eth_sel),
        .o_dev_addr   (eth_dev_addr),
        .o_dev_wdata  (eth_dev_wdata),
        .o_dev_byte_en(eth_dev_byte_en),
        .o_dev_we     (eth_dev_we),
        .o_dev_re     (eth_dev_re),
        .i_dev_rdata  (eth_dev_rdata),
        .i_dev_ack    (eth_dev_ack)
    );

    // The device on the board's PHY. The PHY is reset with the board and
    // by nothing else: a machine restarted any other way finds it as its
    // driver left it.
    nexysvideo_eth #(
        .CLK_HZ     (int'(CPU_HZ)),
        .RX_SLOTS   (ETH_RX_SLOTS),
        .TX_SLOTS   (ETH_TX_SLOTS),
        .SLOT_BYTES (ETH_SLOT_BYTES)
    ) u_eth (
        .i_clk           (cpu_clk),
        .i_rst           (rst),
        .i_clk_100mhz    (clk),
        .i_board_rst     (board_rst),
        .o_phy_ready     (phy_ready),

        .i_addr          (eth_dev_addr),
        .i_wdata         (eth_dev_wdata),
        .i_byte_en       (eth_dev_byte_en),
        .i_we            (eth_dev_we),
        .i_re            (eth_dev_re),
        .o_rdata         (eth_dev_rdata),
        .o_ack           (eth_dev_ack),
        .o_irq           (eth_irq),

        .o_i2c_cmd_valid (i2c_cmd_valid[I2C_EEPROM]),
        .i_i2c_cmd_ready (i2c_cmd_ready[I2C_EEPROM]),
        .o_i2c_cmd_start (i2c_cmd_start[I2C_EEPROM]),
        .o_i2c_cmd_stop  (i2c_cmd_stop[I2C_EEPROM]),
        .o_i2c_cmd_read  (i2c_cmd_read[I2C_EEPROM]),
        .o_i2c_cmd_wdata (i2c_cmd_wdata[I2C_EEPROM]),
        .o_i2c_cmd_nack  (i2c_cmd_nack[I2C_EEPROM]),
        .i_i2c_rsp_valid (i2c_rsp_valid[I2C_EEPROM]),
        .i_i2c_rsp_rdata (i2c_rsp_rdata),
        .i_i2c_rsp_nack  (i2c_rsp_nack),
        .i_i2c_rsp_error (i2c_rsp_error),

        .i_rgmii_rxc     (eth_rxck),
        .i_rgmii_rxd     (eth_rxd),
        .i_rgmii_rx_ctl  (eth_rxctl),
        .o_rgmii_txc     (eth_txck),
        .o_rgmii_txd     (eth_txd),
        .o_rgmii_tx_ctl  (eth_txctl),
        .o_mdc           (eth_mdc),
        .io_mdio         (eth_mdio),
        .o_phy_rst_n     (eth_rst_b)
    );

    // ── PS/2 port (fifth in chain) ──────────────────────────
    // The board's USB HID socket belongs to its PIC24, which presents
    // the keyboard or mouse plugged in there as a PS/2 device on two
    // pins.
    logic [31:0] ps2_dev_addr, ps2_dev_wdata, ps2_dev_rdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  ps2_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        ps2_dev_we, ps2_dev_re, ps2_dev_ack;
    logic [31:0] ac_ps2_rdata;
    logic        ac_ps2_ack, ac_ps2_sel;
    // Last device in the chain: the dangling cfg_out is the
    // "no more devices" end the ROM's autoconfig loop probes.
    /* verilator lint_off UNUSEDSIGNAL */
    logic        ac_ps2_cfg_out;
    /* verilator lint_on UNUSEDSIGNAL */

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_PS2),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00325350)      // "PS2\0" packed LE
    ) u_ac_ps2 (
        .i_clk        (cpu_clk),
        .i_rst        (rst),
        .i_bus_rst    (busctl_bus_rst),
        .i_cfg_en     (busctl_cfg_en),
        .i_cfg_in     (ac_eth_cfg_out),
        .o_cfg_out    (ac_ps2_cfg_out),
        .i_addr       (mem_addr),
        .i_wdata      (mem_wdata),
        .i_byte_en    (mem_byte_en),
        .i_we         (mem_we),
        .i_re         (mem_re),
        .o_rdata      (ac_ps2_rdata),
        .o_ack        (ac_ps2_ack),
        .o_sel        (ac_ps2_sel),
        .o_dev_addr   (ps2_dev_addr),
        .o_dev_wdata  (ps2_dev_wdata),
        .o_dev_byte_en(ps2_dev_byte_en),
        .o_dev_we     (ps2_dev_we),
        .o_dev_re     (ps2_dev_re),
        .i_dev_rdata  (ps2_dev_rdata),
        .i_dev_ack    (ps2_dev_ack)
    );

    logic ps2_clk_release, ps2_data_release;

    ps2 #(
        .CLK_HZ (int'(CPU_HZ))
    ) u_ps2 (
        .i_clk      (cpu_clk),
        .i_rst      (rst),
        .i_addr     (ps2_dev_addr),
        .i_wdata    (ps2_dev_wdata),
        .i_we       (ps2_dev_we),
        .i_re       (ps2_dev_re),
        .o_rdata    (ps2_dev_rdata),
        .o_ack      (ps2_dev_ack),
        .o_irq      (ps2_irq),
        .o_ps2_clk  (ps2_clk_release),
        .i_ps2_clk  (ps2_clk),
        .o_ps2_data (ps2_data_release),
        .i_ps2_data (ps2_data)
    );

    // PS/2 pads, open drain. Nothing on the board pulls these lines
    // up: the pins' own pull-ups make a line high (the constraints
    // enable them).
    assign ps2_clk  = ps2_clk_release  ? 1'bz : 1'b0;
    assign ps2_data = ps2_data_release ? 1'bz : 1'b0;

    // ── Response fabric ─────────────────────────────────────
    localparam int SLV_RAM   = 0;
    localparam int SLV_ROM   = 1;
    localparam int SLV_UART  = 2;
    localparam int SLV_SPI   = 3;
    localparam int SLV_DISP  = 4;
    localparam int SLV_AUDIO = 5;
    localparam int SLV_ETH   = 6;
    localparam int SLV_PS2   = 7;
    localparam int NUM_SLV   = 8;

    logic [NUM_SLV-1:0]       slv_sel;
    logic [NUM_SLV-1:0]       slv_ack;
    logic [NUM_SLV-1:0][31:0] slv_rdata;

    assign slv_sel[SLV_RAM]    = ram_sel;
    assign slv_ack[SLV_RAM]    = ram_ack;
    assign slv_rdata[SLV_RAM]  = ram_rdata;

    assign slv_sel[SLV_ROM]    = rom_sel;
    assign slv_ack[SLV_ROM]    = rom_ack;
    assign slv_rdata[SLV_ROM]  = rom_rdata;

    assign slv_sel[SLV_UART]   = uart_sel;
    assign slv_ack[SLV_UART]   = uart_ack;
    assign slv_rdata[SLV_UART] = uart_rdata;

    assign slv_sel[SLV_SPI]    = ac_spi_sel;
    assign slv_ack[SLV_SPI]    = ac_spi_ack;
    assign slv_rdata[SLV_SPI]  = ac_spi_rdata;

    assign slv_sel[SLV_DISP]   = ac_disp_sel;
    assign slv_ack[SLV_DISP]   = ac_disp_ack;
    assign slv_rdata[SLV_DISP] = ac_disp_rdata;

    assign slv_sel[SLV_AUDIO]   = ac_audio_sel;
    assign slv_ack[SLV_AUDIO]   = ac_audio_ack;
    assign slv_rdata[SLV_AUDIO] = ac_audio_rdata;

    assign slv_sel[SLV_ETH]     = ac_eth_sel;
    assign slv_ack[SLV_ETH]     = ac_eth_ack;
    assign slv_rdata[SLV_ETH]   = ac_eth_rdata;

    assign slv_sel[SLV_PS2]     = ac_ps2_sel;
    assign slv_ack[SLV_PS2]     = ac_ps2_ack;
    assign slv_rdata[SLV_PS2]   = ac_ps2_rdata;

    bus_response_fabric #(
        .NUM_SLAVES     (NUM_SLV),
        .TIMEOUT_CYCLES (BUS_TIMEOUT_CYCLES)
    ) u_bus_fabric (
        .i_clk   (cpu_clk),
        .i_rst   (rst),
        .i_re    (mem_re),
        .i_we    (mem_we),
        .i_sel   (slv_sel),
        .i_ack   (slv_ack),
        .i_rdata (slv_rdata),
        .o_ack   (mem_ack),
        .o_rdata (mem_rdata),
        .o_fault (bus_fault)
    );

    // ── Heartbeat ───────────────────────────────────────────
    localparam int                BEAT_HALF = int'(CPU_HZ / 2);
    localparam int                BEAT_W    = $clog2(BEAT_HALF);
    localparam logic [BEAT_W-1:0] BEAT_LAST = BEAT_W'(BEAT_HALF - 1);

    logic [BEAT_W-1:0] beat_cnt_q;
    logic              beat_q;

    always_ff @(posedge cpu_clk) begin
        if (beat_cnt_q == BEAT_LAST) begin
            beat_cnt_q <= '0;
            beat_q     <= !beat_q;
        end else begin
            beat_cnt_q <= beat_cnt_q + BEAT_W'(1);
        end
    end

    // The RAM's longest wait against the watchdog's limit.
    logic ram_wait_eighth, ram_wait_quarter, ram_wait_half;

    assign ram_wait_eighth  = ram_wait_max >= WAIT_W'(BUS_TIMEOUT_CYCLES / 8);
    assign ram_wait_quarter = ram_wait_max >= WAIT_W'(BUS_TIMEOUT_CYCLES / 4);
    assign ram_wait_half    = ram_wait_max >= WAIT_W'(BUS_TIMEOUT_CYCLES / 2);

    assign led = {thermal_warn, ram_wait_half, ram_wait_quarter, ram_wait_eighth,
                  calib_sync2, beat_q, !rst, locked};

endmodule
