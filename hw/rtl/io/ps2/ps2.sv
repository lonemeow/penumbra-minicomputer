// Penumbra PS/2 port
//
// A port for one PS/2 device on the bus (CLASS_PS2,
// doc/system/devices/ps2.md): the register block, over the link to the
// device (ps2_link). Board-neutral: the two lines leave as levels asked
// for and levels read, for a board to make open-drain pads of.
//
// A simple bus device. A read of RX_DATA takes the waiting byte in the
// access's first cycle, the cycle its value is captured for the answer.
// Both interrupt sources are conditions, with nothing to acknowledge.

module ps2
    import penumbra_pkg::*;
#(
    parameter int CLK_HZ    // i_clk's rate; the integration states it
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Bus (behind autoconfig_dev) ──────────────────────────
    // Only the low offset bits and a few data bits are used here.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    // ── Pins: levels asked for and levels read, as ps2_link's ──
    output logic        o_ps2_clk,
    input  logic        i_ps2_clk,
    output logic        o_ps2_data,
    input  logic        i_ps2_data
);

    // ── Address decode ───────────────────────────────────────
    // The register page repeats through the window.
    logic [4:0] reg_off;
    assign reg_off = {i_addr[4:2], 2'b00};

    // ── Bus handshake: a simple device ───────────────────────
    // A write commits on bus_write and a read of RX_DATA takes its byte
    // on bus_read, once per access, never on the held strobe.
    logic bus_read, bus_write;

    bus_device_simple u_bus (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (i_re),
        .i_we    (i_we),
        .o_read  (bus_read),
        .o_write (bus_write),
        .o_ack   (o_ack)
    );

    // ── The link ─────────────────────────────────────────────
    logic       en_q;           // CTRL.EN
    logic       rx_valid;       // RX_DATA.VALID
    logic [7:0] rx_byte;
    logic       rx_err;
    logic       tx_busy;        // TX_DATA.BUSY
    logic       tx_err;

    logic rx_take, tx_start;
    assign rx_take  = bus_read  && (reg_off == PS2_REG_RX_DATA);
    assign tx_start = bus_write && (reg_off == PS2_REG_TX_DATA);

    ps2_link #(
        .CLK_HZ (CLK_HZ)
    ) u_link (
        .i_clk      (i_clk),
        .i_en       (en_q),
        .o_rx_valid (rx_valid),
        .o_rx_byte  (rx_byte),
        .o_rx_err   (rx_err),
        .i_rx_take  (rx_take),
        .i_tx_start (tx_start),
        .i_tx_byte  (i_wdata[7:0]),
        .o_tx_busy  (tx_busy),
        .o_tx_err   (tx_err),
        .o_ps2_clk  (o_ps2_clk),
        .i_ps2_clk  (i_ps2_clk),
        .o_ps2_data (o_ps2_data),
        .i_ps2_data (i_ps2_data)
    );

    // ── The registers ────────────────────────────────────────
    logic [1:0] irq_enable_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            en_q         <= 1'b0;
            irq_enable_q <= '0;
        end else if (bus_write) begin
            if (reg_off == PS2_REG_IRQ_ENABLE)
                irq_enable_q <= i_wdata[1:0];
            if (reg_off == PS2_REG_CTRL)
                en_q <= i_wdata[0];
        end
    end

    // ── Interrupt ────────────────────────────────────────────
    logic tx_ready;     // IRQ_STATUS.TX: the port can take a byte to send
    assign tx_ready = en_q && !tx_busy;

    assign o_irq = (irq_enable_q[PS2_IRQ_RX] && rx_valid) ||
                   (irq_enable_q[PS2_IRQ_TX] && tx_ready);

    // ── Read data ────────────────────────────────────────────
    logic [31:0] reg_rdata;

    always_comb begin
        case (reg_off)
            PS2_REG_CAP:        reg_rdata = {24'd0, PS2_VERSION};
            PS2_REG_IRQ_STATUS: reg_rdata = {30'd0, tx_ready, rx_valid};
            PS2_REG_IRQ_ENABLE: reg_rdata = {30'd0, irq_enable_q};
            PS2_REG_CTRL:       reg_rdata = {31'd0, en_q};
            PS2_REG_RX_DATA:    reg_rdata = rx_valid ? {22'd0, rx_err, 1'b1, rx_byte} : 32'd0;
            PS2_REG_TX_DATA:    reg_rdata = {22'd0, tx_err, tx_busy, 8'd0};
            default:            reg_rdata = 32'd0;
        endcase
    end

    // Captured in the access's first cycle, presented in the ack cycle.
    logic [31:0] reg_rdata_q;

    always_ff @(posedge i_clk) begin
        reg_rdata_q <= reg_rdata;
    end

    assign o_rdata = reg_rdata_q;

endmodule
