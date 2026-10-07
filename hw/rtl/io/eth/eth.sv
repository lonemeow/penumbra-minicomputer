// Penumbra Ethernet adapter
//
// The CLASS_ETHERNET device (doc/system/devices/ethernet.md), from the
// system bus on one side to the seam a board's PHY adapter attaches at on
// the other (doc/internals/ethernet.md). Nothing here is a board's: the
// PHY adapter, its clocks, the management data pin's pad and the station
// address all come from outside.
//
// Three clock domains meet in this module and nowhere below it. The
// register block and PHY management run on the bus clock, the receiver on
// the receive clock and the transmitter on the transmit clock. Frame bytes
// pass between them through the two slot memories, each with a port in
// two domains, and everything else through one eth_cdc for each PHY-side
// clock, which also gives that side its reset.

module eth #(
    // The bus clock's rate, for PHY management. It has no default: the
    // clock is the integration's, so the integration says what it is.
    parameter int CLK_HZ,
    parameter int RX_SLOTS     = 32,           // slots in the receive ring, a power of two
    parameter int TX_SLOTS     = 8,            // slots in the transmit ring, a power of two
    parameter int SLOT_BYTES   = 2048,         // bytes in a slot, a power of two
    parameter bit FACTORY_ADDR = 1'b1          // the board may supply a station address
)(
    // ── Bus (behind autoconfig_dev) ──────────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    // ── The board's station address, on the bus clock ────────
    input  logic [47:0] i_factory_addr,     // its first byte in [7:0]
    input  logic        i_factory_valid,    // held: rises once and stays

    // ── PHY management pins, on the bus clock ────────────────
    // o_mdio goes onto the data pin while o_mdio_oe is set.
    output logic        o_mdc,
    output logic        o_mdio,
    output logic        o_mdio_oe,
    input  logic        i_mdio,

    // ── The seam ─────────────────────────────────────────────
    input  logic [2:0]  i_speeds,           // the speeds the PHY adapter carries, constant

    input  logic        i_rx_clk,
    input  logic        i_rx_ce,            // a byte time is presented this cycle
    input  logic [7:0]  i_rxd,
    input  logic        i_rx_dv,
    input  logic        i_rx_er,
    output logic [1:0]  o_rx_speed,         // CTRL.SPEED, on the receive clock

    input  logic        i_tx_clk,
    input  logic        i_tx_ce,            // the PHY adapter takes a byte this cycle
    output logic [7:0]  o_txd,
    output logic        o_tx_en,
    output logic [1:0]  o_tx_speed          // CTRL.SPEED, on the transmit clock
);

    localparam int RX_ADDR_W = $clog2(RX_SLOTS * SLOT_BYTES / 4);
    localparam int TX_ADDR_W = $clog2(TX_SLOTS * SLOT_BYTES / 4);

    // What software writes for each side, as that side's crossing carries
    // it: taken whole, so an enable never arrives ahead of the count or
    // the address written with it.
    typedef struct packed {
        logic        enable;        // CTRL.RX_EN
        logic        promisc;
        logic        allmulti;
        logic [1:0]  speed;
        logic [47:0] addr;
        logic [15:0] cons;          // RX_CONS
    } rx_down_t;

    typedef struct packed {
        logic        enable;        // CTRL.TX_EN
        logic [1:0]  speed;
        logic [15:0] prod;          // TX_PROD
    } tx_down_t;

    // ═════════════════════════════════════════════════════════
    //  Bus clock
    // ═════════════════════════════════════════════════════════
    rx_down_t    rx_down;           // from the registers
    tx_down_t    tx_down;
    logic [1:0]  speed;
    logic [15:0] rx_prod, tx_cons;  // the two sides' counts, brought across
    logic        rx_drop;

    logic        mdio_start, mdio_write, mdio_busy, mdio_done;
    logic [4:0]  mdio_phy_addr, mdio_reg_addr;
    logic [15:0] mdio_wdata, mdio_rdata;

    logic [3:0]           tx_mem_we;
    logic [TX_ADDR_W-1:0] tx_mem_waddr;
    logic [31:0]          tx_mem_wdata;
    logic [RX_ADDR_W-1:0] rx_mem_raddr;
    logic [31:0]          rx_mem_rdata;

    assign rx_down.speed = speed;
    assign tx_down.speed = speed;

    eth_regs #(
        .RX_SLOTS     (RX_SLOTS),
        .TX_SLOTS     (TX_SLOTS),
        .SLOT_BYTES   (SLOT_BYTES),
        .FACTORY_ADDR (FACTORY_ADDR)
    ) u_regs (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_addr          (i_addr),
        .i_wdata         (i_wdata),
        .i_byte_en       (i_byte_en),
        .i_we            (i_we),
        .i_re            (i_re),
        .o_rdata         (o_rdata),
        .o_ack           (o_ack),
        .o_irq           (o_irq),
        .i_speeds        (i_speeds),
        .i_factory_addr  (i_factory_addr),
        .i_factory_valid (i_factory_valid),
        .o_rx_en         (rx_down.enable),
        .o_promisc       (rx_down.promisc),
        .o_allmulti      (rx_down.allmulti),
        .o_addr          (rx_down.addr),
        .o_rx_cons       (rx_down.cons),
        .i_rx_prod       (rx_prod),
        .i_rx_drop       (rx_drop),
        .o_tx_en         (tx_down.enable),
        .o_tx_prod       (tx_down.prod),
        .i_tx_cons       (tx_cons),
        .o_speed         (speed),
        .o_mdio_start    (mdio_start),
        .o_mdio_write    (mdio_write),
        .o_mdio_phy_addr (mdio_phy_addr),
        .o_mdio_reg_addr (mdio_reg_addr),
        .o_mdio_wdata    (mdio_wdata),
        .i_mdio_busy     (mdio_busy),
        .i_mdio_done     (mdio_done),
        .i_mdio_rdata    (mdio_rdata),
        .o_tx_mem_we     (tx_mem_we),
        .o_tx_mem_addr   (tx_mem_waddr),
        .o_tx_mem_wdata  (tx_mem_wdata),
        .o_rx_mem_addr   (rx_mem_raddr),
        .i_rx_mem_rdata  (rx_mem_rdata)
    );

    eth_mdio #(
        .CLK_HZ (CLK_HZ)
    ) u_mdio (
        .i_clk      (i_clk),
        .i_start    (mdio_start),
        .i_write    (mdio_write),
        .i_phy_addr (mdio_phy_addr),
        .i_reg_addr (mdio_reg_addr),
        .i_wdata    (mdio_wdata),
        .o_busy     (mdio_busy),
        .o_done     (mdio_done),
        .o_rdata    (mdio_rdata),
        .o_mdc      (o_mdc),
        .o_mdio     (o_mdio),
        .o_mdio_oe  (o_mdio_oe),
        .i_mdio     (i_mdio)
    );

    // ═════════════════════════════════════════════════════════
    //  Receive clock
    // ═════════════════════════════════════════════════════════
    logic        rx_rst;
    rx_down_t    rx_ctl;            // rx_down, on this clock
    logic [15:0] rx_prod_dev;       // the receiver's own count
    logic        rx_drop_dev;

    logic                 rx_mem_we;
    logic [RX_ADDR_W-1:0] rx_mem_waddr;
    logic [31:0]          rx_mem_wdata;

    eth_cdc #(
        .DOWN_W ($bits(rx_down_t))
    ) u_rx_cdc (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_down     (rx_down),
        .o_up_count (rx_prod),
        .o_up_pulse (rx_drop),
        .i_dev_clk  (i_rx_clk),
        .o_dev_rst  (rx_rst),
        .o_down     (rx_ctl),
        .i_up_count (rx_prod_dev),
        .i_up_pulse (rx_drop_dev)
    );

    assign o_rx_speed = rx_ctl.speed;

    eth_rx #(
        .SLOTS      (RX_SLOTS),
        .SLOT_BYTES (SLOT_BYTES)
    ) u_rx (
        .i_clk       (i_rx_clk),
        .i_rst       (rx_rst),
        .i_enable    (rx_ctl.enable),
        .i_promisc   (rx_ctl.promisc),
        .i_allmulti  (rx_ctl.allmulti),
        .i_addr      (rx_ctl.addr),
        .i_cons      (rx_ctl.cons),
        .o_prod      (rx_prod_dev),
        .o_drop      (rx_drop_dev),
        .o_mem_we    (rx_mem_we),
        .o_mem_addr  (rx_mem_waddr),
        .o_mem_wdata (rx_mem_wdata),
        .i_ce        (i_rx_ce),
        .i_rxd       (i_rxd),
        .i_dv        (i_rx_dv),
        .i_er        (i_rx_er)
    );

    eth_slot_ram #(
        .WORDS (RX_SLOTS * SLOT_BYTES / 4)
    ) u_rx_slots (
        .i_wclk  (i_rx_clk),
        .i_we    ({4{rx_mem_we}}),
        .i_waddr (rx_mem_waddr),
        .i_wdata (rx_mem_wdata),
        .i_rclk  (i_clk),
        .i_raddr (rx_mem_raddr),
        .o_rdata (rx_mem_rdata)
    );

    // ═════════════════════════════════════════════════════════
    //  Transmit clock
    // ═════════════════════════════════════════════════════════
    logic        tx_rst;
    tx_down_t    tx_ctl;            // tx_down, on this clock
    logic [15:0] tx_cons_dev;       // the transmitter's own count

    logic [TX_ADDR_W-1:0] tx_mem_raddr;
    logic [31:0]          tx_mem_rdata;

    // The transmitter sends nothing up but its count.
    /* verilator lint_off PINCONNECTEMPTY */
    eth_cdc #(
        .DOWN_W ($bits(tx_down_t))
    ) u_tx_cdc (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_down     (tx_down),
        .o_up_count (tx_cons),
        .o_up_pulse (),
        .i_dev_clk  (i_tx_clk),
        .o_dev_rst  (tx_rst),
        .o_down     (tx_ctl),
        .i_up_count (tx_cons_dev),
        .i_up_pulse (1'b0)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    assign o_tx_speed = tx_ctl.speed;

    eth_tx #(
        .SLOTS      (TX_SLOTS),
        .SLOT_BYTES (SLOT_BYTES)
    ) u_tx (
        .i_clk       (i_tx_clk),
        .i_rst       (tx_rst),
        .i_enable    (tx_ctl.enable),
        .i_prod      (tx_ctl.prod),
        .o_cons      (tx_cons_dev),
        .o_mem_addr  (tx_mem_raddr),
        .i_mem_rdata (tx_mem_rdata),
        .i_ce        (i_tx_ce),
        .o_txd       (o_txd),
        .o_tx_en     (o_tx_en)
    );

    eth_slot_ram #(
        .WORDS (TX_SLOTS * SLOT_BYTES / 4)
    ) u_tx_slots (
        .i_wclk  (i_clk),
        .i_we    (tx_mem_we),
        .i_waddr (tx_mem_waddr),
        .i_wdata (tx_mem_wdata),
        .i_rclk  (i_tx_clk),
        .i_raddr (tx_mem_raddr),
        .o_rdata (tx_mem_rdata)
    );

endmodule
