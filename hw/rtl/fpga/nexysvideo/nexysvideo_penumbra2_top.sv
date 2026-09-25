// Nexys Video board top (Penumbra/2): the gen2 machine on the XC7A200T
//
// Wires machine_penumbra2 to the board: the shared clocking and power-on
// reset (nexysvideo_clk_rst), a 64 KB boot ROM, the UART on the UART USB
// port, the microSD slot over SPI behind autoconfig, and system RAM in
// block RAM until the DDR3 arrives. The top grows
// one device per bring-up step; the external Penumbra Bus is assembled
// here the way the ULX3S top assembles it (device decode, response fabric
// with its no-device watchdog), so what simulates is what synthesizes.
//
// The LEDs report the board-level pieces under the machine: led[0] MMCM
// locked, led[1] reset released, led[2] a 1 Hz heartbeat counted off the
// CPU clock, led[7] the die above the thermal guard's warning threshold.
// The console is 115200 8N1.

// CPU microarchitecture variant: 0 = gen2 baseline, 1 = gen2.5
// (forwarding/prediction). Set by the build (sv2v -D) per CORE; the
// gen2.5 artifact synthesizes this same module with the bit flipped.
`ifndef PENUMBRA_CPU_VARIANT
`define PENUMBRA_CPU_VARIANT 0
`endif

module nexysvideo_penumbra2_top (
    input  logic       clk,          // 100 MHz oscillator
    input  logic       cpu_resetn,   // CPU RESET button, low while pressed
    output logic [7:0] led,
    output logic       uart_rx_out,  // FPGA TX, to the host's RX
    input  logic       uart_tx_in,   // FPGA RX, from the host's TX

    // ── microSD slot, in SPI mode ──────────────────────────
    output logic       sd_cclk,      // SCK
    output logic       sd_cmd,       // MOSI
    inout  wire  [3:0] sd_d,         // [0] MISO in, [3] CS out, [2:1] held high
    output logic       sd_reset      // card power: low powers the slot
);
    import penumbra_pkg::*;

    localparam longint CPU_HZ    = 60_000_000;
    localparam int     RAM_WORDS = 32768;       // 128 KiB
    localparam int     ROM_WORDS = 16384;       // 64 KiB

    logic cpu_clk;
    logic locked;
    logic rst;

    nexysvideo_clk_rst #(
        .CPU_HZ (CPU_HZ)
    ) u_clk_rst (
        .i_clk_100mhz (clk),
        .i_cpu_resetn (cpu_resetn),
        .o_clk        (cpu_clk),
        .o_locked     (locked),
        .o_rst        (rst)
    );

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

    // ── Autoconfig control from the machine's bus controller ──
    logic        busctl_bus_rst;
    logic        busctl_cfg_en;

    machine_penumbra2 #(
        .CPU_VARIANT(`PENUMBRA_CPU_VARIANT),
        .MACH_NAME0 (32'h7978654E),   // "Nexy"
        .MACH_NAME1 (32'h69562073),   // "s Vi"
        .MACH_NAME2 (32'h006F6564),   // "deo\0"
        .CPU_FREQ   (32'(CPU_HZ))
    ) u_machine (
        .i_clk         (cpu_clk),
        .i_rst         (rst),

        .i_irq         (uart_irq | spi_irq),
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

        // Commit / retire / trace observability, unused on the board.
        /* verilator lint_off PINCONNECTEMPTY */
        .o_commit_idx        (),
        .o_commit_data       (),
        .o_commit_we         (),
        .o_retire_valid      (),
        .o_retire_op_class   (),
        .o_retire_pc         (),
        .o_retire_sr         (),
        .o_fault_commit      (),
        .o_fault_vec         (),
        .o_eret_commit       (),
        .o_dc_commit         (),
        .o_dc_commit_pc      (),
        .o_dc_commit_op_class(),
        .o_branch_taken      (),
        .o_branch_target     (),
        .o_branch_pc         (),
        .o_ex_pc             (),
        .o_ex_valid          (),
        .o_mem_pc            (),
        .o_mem_valid         (),
        .o_prog_end          ()
        /* verilator lint_on PINCONNECTEMPTY */
    );

    // ── System RAM, in block RAM ────────────────────────────
    logic        ram_sel;
    logic [31:0] ram_rdata;
    logic        ram_ack;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));

    onchip_ram #(
        .WORDS (RAM_WORDS)
    ) u_ram (
        .i_clk     (cpu_clk),
        .i_rst     (rst),
        .i_addr    (mem_addr),
        .i_wdata   (mem_wdata),
        .i_byte_en (mem_byte_en),
        .i_re      (mem_re & ram_sel),
        .i_we      (mem_we & ram_sel),
        .o_rdata   (ram_rdata),
        .o_ack     (ram_ack)
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
    logic        ac_spi_cfg_out;       // the chain's next link, when it has one
    /* verilator lint_on UNUSEDSIGNAL */
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

    // ── Response fabric ─────────────────────────────────────
    localparam int SLV_RAM  = 0;
    localparam int SLV_ROM  = 1;
    localparam int SLV_UART = 2;
    localparam int SLV_SPI  = 3;
    localparam int NUM_SLV  = 4;

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

    bus_response_fabric #(
        .NUM_SLAVES (NUM_SLV)
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

    assign led = {thermal_warn, 4'b0000, beat_q, !rst, locked};

endmodule
