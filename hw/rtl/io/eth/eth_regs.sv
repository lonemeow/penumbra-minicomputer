// Penumbra Ethernet adapter register block
//
// The adapter's bus side (doc/system/devices/ethernet.md): the register
// page, the two slot apertures and the interrupt, all on the bus clock.
// What software writes for the receiver and the transmitter leaves here
// as plain values for the adapter's crossings to carry, and each side's
// own count comes back the same way.
//
// A simple bus device: every access does its work in its first cycle and
// is acknowledged in the next. The slot memories register their reads, so
// a read of the receive aperture has its data in the ack cycle as a
// register's does.
//
// Access widths are the contract's: the registers are word-strided, and
// the byte lanes mean something only in the transmit aperture, where a
// write commits exactly the lanes it enables.

module eth_regs
    import penumbra_pkg::*;
#(
    parameter int RX_SLOTS     = 32,    // slots in the receive ring, a power of two
    parameter int TX_SLOTS     = 8,     // slots in the transmit ring, a power of two
    parameter int SLOT_BYTES   = 2048,  // bytes in a slot, a power of two
    parameter bit FACTORY_ADDR = 1'b1   // the board may supply a station address: CAP.ADDR
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Bus (behind autoconfig_dev) ──────────────────────────
    // The window's offset bits carry the decode; the bits above them
    // are the wrapper's concern.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    // ── What the board gives the device ──────────────────────
    input  logic [2:0]  i_speeds,           // the speeds its PHY adapter carries: CAP.SPEEDS
    input  logic [47:0] i_factory_addr,     // its station address, the first byte in [7:0]
    input  logic        i_factory_valid,    // and whether that is held yet

    // ── For the receiver, and its count back ─────────────────
    output logic        o_rx_en,
    output logic        o_promisc,
    output logic        o_allmulti,
    output logic [47:0] o_addr,
    output logic [15:0] o_rx_cons,
    input  logic [15:0] i_rx_prod,
    input  logic        i_rx_drop,          // one cycle for each frame lost to a full ring

    // ── For the transmitter, and its count back ──────────────
    output logic        o_tx_en,
    output logic [15:0] o_tx_prod,
    input  logic [15:0] i_tx_cons,

    // ── For both, and the PHY adapter ────────────────────────
    output logic [1:0]  o_speed,            // CTRL.SPEED

    // ── PHY management (eth_mdio) ────────────────────────────
    output logic        o_mdio_start,
    output logic        o_mdio_write,
    output logic [4:0]  o_mdio_phy_addr,
    output logic [4:0]  o_mdio_reg_addr,
    output logic [15:0] o_mdio_wdata,
    input  logic        i_mdio_busy,
    input  logic        i_mdio_done,
    input  logic [15:0] i_mdio_rdata,

    // ── The transmit slots' write port ───────────────────────
    // A word address across the ring, and a write enable for each byte
    // lane.
    output logic [3:0]  o_tx_mem_we,
    output logic [$clog2(TX_SLOTS * SLOT_BYTES / 4)-1:0] o_tx_mem_addr,
    output logic [31:0] o_tx_mem_wdata,

    // ── The receive slots' read port ─────────────────────────
    // i_rx_mem_rdata is the word at the address of the cycle before.
    output logic [$clog2(RX_SLOTS * SLOT_BYTES / 4)-1:0] o_rx_mem_addr,
    input  logic [31:0] i_rx_mem_rdata
);

    localparam int TX_BYTES  = TX_SLOTS * SLOT_BYTES;
    localparam int RX_BYTES  = RX_SLOTS * SLOT_BYTES;
    localparam int TX_ADDR_W = $clog2(TX_BYTES / 4);
    localparam int RX_ADDR_W = $clog2(RX_BYTES / 4);

    // The device window reaches the end of the receive ring.
    localparam int OFFSET_W = $clog2(ETH_RX_APERTURE + RX_BYTES);

    build_check #(
        .COND (TX_BYTES <= ETH_RX_APERTURE - ETH_TX_APERTURE),
        .MSG  ("eth_regs: the transmit ring does not fit below the receive aperture")
    ) u_check_tx_fits ();

    build_check #(
        .COND ((RX_SLOTS & (RX_SLOTS - 1)) == 0 && (TX_SLOTS & (TX_SLOTS - 1)) == 0 &&
               (SLOT_BYTES & (SLOT_BYTES - 1)) == 0 && SLOT_BYTES >= 2048 &&
               SLOT_BYTES < 65536),
        .MSG  ("eth_regs: the rings' geometry is not powers of two that GEOM can state")
    ) u_check_geometry ();

    // ── Address decode ───────────────────────────────────────
    // The register page at the bottom of the window, the transmit
    // aperture above it, and the receive aperture from there to the top.
    //
    // Only the offsets the interface names are decoded for what they are.
    // Software keeps to those (doc/hardware/bus-protocol.md, "Access
    // Width"), so the rest of the register region repeats the register
    // page and the space past the transmit ring repeats the ring.
    logic [OFFSET_W-1:0] offset;
    assign offset = i_addr[OFFSET_W-1:0];

    logic rx_sel, tx_sel, reg_sel;
    assign rx_sel  = (offset >= OFFSET_W'(ETH_RX_APERTURE));
    assign tx_sel  = !rx_sel && (offset >= OFFSET_W'(ETH_TX_APERTURE));
    assign reg_sel = !rx_sel && !tx_sel;

    logic [6:0] reg_off;
    assign reg_off = {offset[6:2], 2'b00};

    // ── Bus handshake: a simple device ───────────────────────
    // Every write commits on bus_write, once per access, never on the
    // held strobe. Reads have no side effects and need only the ack.
    logic bus_write;

    bus_device_simple u_bus (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (i_re),
        .i_we    (i_we),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_read  (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_write (bus_write),
        .o_ack   (o_ack)
    );

    logic reg_write;
    assign reg_write = bus_write && reg_sel;

    // ── The slot apertures ───────────────────────────────────
    assign o_tx_mem_we    = (bus_write && tx_sel) ? i_byte_en : 4'b0000;
    assign o_tx_mem_addr  = TX_ADDR_W'((offset - OFFSET_W'(ETH_TX_APERTURE)) >> 2);
    assign o_tx_mem_wdata = i_wdata;

    assign o_rx_mem_addr  = RX_ADDR_W'((offset - OFFSET_W'(ETH_RX_APERTURE)) >> 2);

    // ── The registers ────────────────────────────────────────
    logic [1:0]  irq_enable_q;
    logic        rx_en_q, tx_en_q, promisc_q, allmulti_q;
    logic [1:0]  speed_q;
    logic [47:0] addr_q;
    logic [15:0] rx_cons_q, tx_prod_q, tx_wm_q;
    logic [31:0] rx_drop_q;

    // MDIO's fields as written. A read's answer takes the place of DATA.
    logic        mdio_write_q;
    logic [4:0]  mdio_phy_addr_q, mdio_reg_addr_q;
    logic [15:0] mdio_data_q;

    assign o_rx_en    = rx_en_q;
    assign o_tx_en    = tx_en_q;
    assign o_promisc  = promisc_q;
    assign o_allmulti = allmulti_q;
    assign o_speed    = speed_q;
    assign o_addr     = addr_q;
    assign o_rx_cons  = rx_cons_q;
    assign o_tx_prod  = tx_prod_q;

    // The transaction leaves from the bus's own data, in the cycle the
    // write commits, and the fields are kept for reading back.
    logic mdio_start;
    assign mdio_start = reg_write && (reg_off == ETH_REG_MDIO) && !i_mdio_busy;

    assign o_mdio_start    = mdio_start;
    assign o_mdio_write    = i_wdata[30];
    assign o_mdio_phy_addr = i_wdata[28:24];
    assign o_mdio_reg_addr = i_wdata[20:16];
    assign o_mdio_wdata    = i_wdata[15:0];

    // CTRL.SPEED resets to the slowest speed the device carries.
    logic [1:0] speed_at_reset;
    assign speed_at_reset = i_speeds[0] ? 2'd0 : i_speeds[1] ? 2'd1 : 2'd2;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            irq_enable_q    <= '0;
            rx_en_q         <= 1'b0;
            tx_en_q         <= 1'b0;
            promisc_q       <= 1'b0;
            allmulti_q      <= 1'b0;
            speed_q         <= speed_at_reset;
            addr_q          <= '0;
            rx_cons_q       <= '0;
            tx_prod_q       <= '0;
            tx_wm_q         <= '0;
            rx_drop_q       <= '0;
            mdio_write_q    <= 1'b0;
            mdio_phy_addr_q <= '0;
            mdio_reg_addr_q <= '0;
            mdio_data_q     <= '0;
        end else begin
            if (i_rx_drop)
                rx_drop_q <= rx_drop_q + 32'd1;

            if (i_mdio_done && !mdio_write_q)
                mdio_data_q <= i_mdio_rdata;

            if (mdio_start) begin
                mdio_write_q    <= i_wdata[30];
                mdio_phy_addr_q <= i_wdata[28:24];
                mdio_reg_addr_q <= i_wdata[20:16];
                mdio_data_q     <= i_wdata[15:0];
            end

            if (reg_write) begin
                unique case (reg_off)
                    ETH_REG_IRQ_ENABLE: irq_enable_q <= i_wdata[1:0];
                    ETH_REG_CTRL: begin
                        rx_en_q    <= i_wdata[0];
                        tx_en_q    <= i_wdata[1];
                        promisc_q  <= i_wdata[2];
                        allmulti_q <= i_wdata[3];
                        speed_q    <= i_wdata[9:8];
                    end
                    ETH_REG_ADDR_LO: addr_q[31:0]  <= i_wdata;
                    ETH_REG_ADDR_HI: addr_q[47:32] <= i_wdata[15:0];
                    ETH_REG_RX_CONS: rx_cons_q     <= i_wdata[15:0];
                    ETH_REG_TX_PROD: tx_prod_q     <= i_wdata[15:0];
                    ETH_REG_TX_WM:   tx_wm_q       <= i_wdata[15:0];
                    default: ;
                endcase
            end
        end
    end

    // ── Interrupt ────────────────────────────────────────────
    // Both sources are conditions on the counts, with nothing to
    // acknowledge: each ends when software changes what it is compared
    // with.
    logic rx_not_empty;     // IRQ_STATUS.RX: the receive ring holds frames not yet released
    logic tx_below_wm;      // IRQ_STATUS.TX: fewer frames are queued than TX_WM

    assign rx_not_empty = (rx_cons_q != i_rx_prod);
    assign tx_below_wm  = (tx_prod_q - i_tx_cons) < tx_wm_q;
    assign o_irq        = (irq_enable_q[ETH_IRQ_RX] && rx_not_empty) ||
                          (irq_enable_q[ETH_IRQ_TX] && tx_below_wm);

    // ── Read data ────────────────────────────────────────────
    logic [31:0] reg_rdata;

    always_comb begin
        reg_rdata = 32'd0;
        if (reg_sel) begin
            unique case (reg_off)
                ETH_REG_CAP:        reg_rdata = {13'd0, i_speeds, 6'd0, FACTORY_ADDR, 1'b1,
                                                 ETH_VERSION};
                ETH_REG_IRQ_STATUS: reg_rdata = {30'd0, tx_below_wm, rx_not_empty};
                ETH_REG_IRQ_ENABLE: reg_rdata = {30'd0, irq_enable_q};
                ETH_REG_CTRL:       reg_rdata = {22'd0, speed_q, 4'd0, allmulti_q, promisc_q,
                                                 tx_en_q, rx_en_q};
                ETH_REG_MDIO:       reg_rdata = {i_mdio_busy, mdio_write_q, 1'b0,
                                                 mdio_phy_addr_q, 3'd0, mdio_reg_addr_q,
                                                 mdio_data_q};
                ETH_REG_ADDR_LO:    reg_rdata = addr_q[31:0];
                ETH_REG_ADDR_HI:    reg_rdata = {16'd0, addr_q[47:32]};
                ETH_REG_FACTORY_LO: reg_rdata = FACTORY_ADDR ? i_factory_addr[31:0] : 32'd0;
                ETH_REG_FACTORY_HI: reg_rdata = FACTORY_ADDR
                                              ? {i_factory_valid, 15'd0, i_factory_addr[47:32]}
                                              : 32'd0;
                ETH_REG_RX_GEOM:    reg_rdata = {16'(SLOT_BYTES), 16'(RX_SLOTS)};
                ETH_REG_RX_PROD:    reg_rdata = {16'd0, i_rx_prod};
                ETH_REG_RX_CONS:    reg_rdata = {16'd0, rx_cons_q};
                ETH_REG_RX_DROP:    reg_rdata = rx_drop_q;
                ETH_REG_TX_GEOM:    reg_rdata = {16'(SLOT_BYTES), 16'(TX_SLOTS)};
                ETH_REG_TX_PROD:    reg_rdata = {16'd0, tx_prod_q};
                ETH_REG_TX_CONS:    reg_rdata = {16'd0, i_tx_cons};
                ETH_REG_TX_WM:      reg_rdata = {16'd0, tx_wm_q};
                default:            reg_rdata = 32'd0;
            endcase
        end
    end

    // The memory registers the receive aperture's read. The registers'
    // is registered here, beside the choice between the two, so both
    // answer in the ack cycle. A read of the transmit aperture, or of
    // anything else, is the register side reading zero.
    logic [31:0] reg_rdata_q;
    logic        rx_sel_q;

    always_ff @(posedge i_clk) begin
        reg_rdata_q <= reg_rdata;
        rx_sel_q    <= rx_sel;
    end

    assign o_rdata = rx_sel_q ? i_rx_mem_rdata : reg_rdata_q;

endmodule
