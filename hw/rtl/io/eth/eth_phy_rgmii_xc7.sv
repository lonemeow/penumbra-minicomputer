// Penumbra Ethernet PHY adapter: RGMII at 10 and 100 Mbit/s, 7-series I/O
//
// Presents an RGMII PHY at the Ethernet adapter's seam
// (doc/internals/ethernet.md, "The MAC-PHY Seam"), every pin registered
// in its I/O cell. It carries 10 and 100 Mbit/s, where RGMII moves a
// nibble in each cycle of a 2.5 or 25 MHz clock, low nibble first, and
// needs neither a delay cell nor a clock manager for it. What 1000 would
// take, and why the board does without, is in
// doc/internals/nexysvideo/ethernet.md.
//
// A packet, here as in 802.3, is a preamble, the start delimiter and then
// the frame. RGMII's data valid and transmit enable mark a packet's
// nibbles, the preamble's and the delimiter's among them, and are clear
// in the gap between packets.
//
// Receive. The receive clock is the PHY's, through a global buffer. Each
// input cell samples its pin on both of that clock's edges: the rising
// edge finds the nibble and data valid, and the control pin's falling
// edge differs from its rising one when the PHY reports an error in the
// nibble. The PHY changes its pins on the clock's own edges,
// and the samples are right because the clock reaches the input cells
// later than the pins' values do. A board's constraints have to state
// the pins as changing on the clock's edges, so that the tools check
// that and do not assume it.
//
// A PHY may hand over any number of the preamble's nibbles, odd or even,
// so the adapter does not pair nibbles from where data valid rises. It
// presents nothing until the start delimiter's two nibbles have come,
// presents that byte, and pairs from there. Between packets it presents
// a byte time with data valid clear on every cycle.
//
// The delimiter alone marks where a frame starts. Whatever else comes
// ahead of it in a packet is passed over, since a PHY may hand over a
// short preamble or a rough first nibble in front of a good frame, and a
// delimiter found in noise leads only to a frame whose check sequence
// fails.
//
// Transmit. The transmit clock is the board's and runs at a multiple of
// 100 MHz, so a quarter of the pin clock's period is a whole number of
// its cycles at either speed. The pin clock is a pattern on its pin, and
// the data and control pins change a quarter period before it rises. The
// PHY reads the control pin on the pin clock's rise for transmit enable
// and on the fall after it for an error, so a nibble has to span a rise
// and then a fall; a quarter period lies between every
// change and both of those edges, in place of the delay RGMII asks one
// side to add to the clock.
//
// The two directions share no signal. Neither has a reset: the receive
// side takes its bearings from the gap before each packet, and the
// transmit side's counters run from whatever they wake with, the pins
// following the transmitter within a byte time.

module eth_phy_rgmii_xc7 #(
    // The transmit clock's rate. It has no default: the clock is the
    // board's, so the board says what it is.
    parameter int unsigned TX_CLK_HZ
)(
    // ── The seam: receive, on o_rx_clk ───────────────────────
    output logic       o_rx_clk,        // the PHY's receive clock
    output logic       o_rx_ce,         // a byte time is presented this cycle
    output logic [7:0] o_rxd,
    output logic       o_rx_dv,         // a packet's byte: its delimiter, or its frame's
    output logic       o_rx_er,         // the PHY found an error in it

    // ── The seam: transmit, on i_tx_clk ──────────────────────
    input  logic       i_tx_clk,        // the device's transmit clock is this one
    output logic       o_tx_ce,         // the byte time put up is taken this cycle
    input  logic [7:0] i_txd,
    input  logic       i_tx_en,         // a packet's byte: preamble, delimiter or frame
    input  logic [1:0] i_tx_speed,      // CTRL.SPEED: 0 for 10 Mbit/s, 1 for 100

    output logic [2:0] o_speeds,        // the speeds carried: 10 and 100

    // ── RGMII pins ───────────────────────────────────────────
    input  logic       i_rgmii_rxc,
    input  logic [3:0] i_rgmii_rxd,
    input  logic       i_rgmii_rx_ctl,
    output logic       o_rgmii_txc,
    output logic [3:0] o_rgmii_txd,
    output logic       o_rgmii_tx_ctl
);

    assign o_speeds = 3'b011;

    // The pin clock's rate at each speed, and the transmit clock's cycles
    // in a quarter of its period.
    localparam int unsigned TXC_HZ_100  = 25_000_000;
    localparam int unsigned TXC_HZ_10   = 2_500_000;
    localparam int unsigned QUARTER_100 = TX_CLK_HZ / (4 * TXC_HZ_100);
    localparam int unsigned QUARTER_10  = TX_CLK_HZ / (4 * TXC_HZ_10);
    localparam int          QUARTER_W   = (QUARTER_10 > 1) ? $clog2(QUARTER_10) : 1;

    // The pin clock's quarters have to be whole cycles of the transmit
    // clock. Zero is what a parameter with no default reads as where a
    // tool elaborates a module's own copy (build_check.sv).
    build_check #(
        .COND (TX_CLK_HZ == 0 || (TX_CLK_HZ % (4 * TXC_HZ_100)) == 0),
        .MSG  ("eth_phy_rgmii_xc7: TX_CLK_HZ is not a multiple of 100 MHz")
    ) u_check_tx_clk ();

    // ═════════════════════════════════════════════════════════
    //  Receive
    // ═════════════════════════════════════════════════════════
    logic rx_clk;

    BUFG u_rxc_bufg (
        .I (i_rgmii_rxc),
        .O (rx_clk)
    );

    assign o_rx_clk = rx_clk;

    // The pins as one cycle of the receive clock found them: both halves
    // of a cycle together, a cycle later.
    logic [3:0] rx_nibble;      // the data pins at the rising edge
    logic       rx_ctl_rise;    // the control pin at the rising edge
    logic       rx_ctl_fall;    // and at the falling edge after it

    // The data pins' falling edge is the other half of a byte only at
    // 1000 Mbit/s.
    /* verilator lint_off PINCONNECTEMPTY */
    for (genvar i = 0; i < 4; i++) begin : g_rxd
        IDDR #(
            .DDR_CLK_EDGE ("SAME_EDGE_PIPELINED")
        ) u_iddr (
            .Q1 (rx_nibble[i]),
            .Q2 (),
            .C  (rx_clk),
            .CE (1'b1),
            .D  (i_rgmii_rxd[i]),
            .R  (1'b0),
            .S  (1'b0)
        );
    end
    /* verilator lint_on PINCONNECTEMPTY */

    IDDR #(
        .DDR_CLK_EDGE ("SAME_EDGE_PIPELINED")
    ) u_rx_ctl_iddr (
        .Q1 (rx_ctl_rise),
        .Q2 (rx_ctl_fall),
        .C  (rx_clk),
        .CE (1'b1),
        .D  (i_rgmii_rx_ctl),
        .R  (1'b0),
        .S  (1'b0)
    );

    logic rx_dv;                // data valid: this cycle's nibble is a packet's
    logic rx_error;             // and the PHY found an error in it
    assign rx_dv    = rx_ctl_rise;
    assign rx_error = rx_ctl_rise ^ rx_ctl_fall;

    // This cycle's nibble over the one before it. Every cycle makes such
    // a pair, and the state says which pairs are bytes.
    logic [3:0] prev_nibble_q;
    logic       prev_error_q;
    logic [7:0] rx_byte;
    assign rx_byte = {rx_nibble, prev_nibble_q};

    typedef enum logic [1:0] {
        S_GAP,          // between packets
        S_PREAMBLE,     // in a packet, its start delimiter still to come
        S_LOW,          // in the frame, at the low half of a byte
        S_HIGH          // in the frame, at the high half
    } state_t;

    state_t state_q;

    always_ff @(posedge rx_clk) begin
        prev_nibble_q <= rx_nibble;
        prev_error_q  <= rx_error;

        // What a byte time holds, whenever there is one.
        o_rx_dv <= rx_dv;
        o_rx_er <= rx_dv && (rx_error || prev_error_q);
        o_rxd   <= rx_byte;

        // When there is one: on every cycle between packets, and within
        // a packet where the state below finds a pair to be a byte.
        o_rx_ce <= !rx_dv;

        if (!rx_dv) begin
            state_q <= S_GAP;
        end else begin
            unique case (state_q)
                // A packet's first nibble has none of its own before it.
                S_GAP: begin
                    state_q <= S_PREAMBLE;
                end

                // Pairs one nibble apart, until one is the start
                // delimiter. Nothing else is asked of a preamble: its
                // length is the PHY's to choose, and a pair that is not
                // 0x55 is passed over like one that is.
                S_PREAMBLE: begin
                    if (rx_byte == 8'hD5) begin
                        o_rx_ce <= 1'b1;
                        state_q <= S_LOW;
                    end
                end

                // Every other pair from there.
                S_LOW: begin
                    state_q <= S_HIGH;
                end

                S_HIGH: begin
                    o_rx_ce <= 1'b1;
                    state_q <= S_LOW;
                end
            endcase
        end
    end

    // ═════════════════════════════════════════════════════════
    //  Transmit
    // ═════════════════════════════════════════════════════════
    // A byte time is eight quarters of the pin clock's period, four to a
    // nibble. quarter_q counts the transmit clock's cycles within a
    // quarter and phase_q the quarters within the byte time.
    logic                 speed_100;
    logic [QUARTER_W-1:0] quarter_top;      // quarter_q in the last cycle of a quarter
    assign speed_100   = (i_tx_speed == 2'd1);
    assign quarter_top = speed_100 ? QUARTER_W'(QUARTER_100 - 1) : QUARTER_W'(QUARTER_10 - 1);

    logic [QUARTER_W-1:0] quarter_q, quarter_d;
    logic [2:0]           phase_q, phase_d;
    logic                 quarter_ends;     // this cycle is a quarter's last

    // At or past the top, so a count left over from the other speed, or
    // from waking, still comes round.
    assign quarter_ends = (quarter_q >= quarter_top);

    always_comb begin
        quarter_d = quarter_ends ? '0 : quarter_q + QUARTER_W'(1);
        phase_d   = quarter_ends ? phase_q + 3'd1 : phase_q;
    end

    // The byte time in hand: the transmitter's as it is taken, and held
    // from there.
    logic [7:0] byte_q, byte_now;
    logic       en_q, en_now;
    assign byte_now = o_tx_ce ? i_txd   : byte_q;
    assign en_now   = o_tx_ce ? i_tx_en : en_q;

    always_ff @(posedge i_tx_clk) begin
        quarter_q <= quarter_d;
        phase_q   <= phase_d;

        // The byte time's last cycle, so that the next begins with the
        // byte taken in this one.
        o_tx_ce <= (phase_d == 3'd7) && (quarter_d >= quarter_top);

        if (o_tx_ce) begin
            byte_q <= i_txd;
            en_q   <= i_tx_en;
        end
    end

    // What each pin shows in the next cycle. A nibble is four quarters:
    // the pins change as its first begins, the pin clock rises as its
    // second begins and falls as its fourth does.
    logic       txc_d;
    logic [3:0] txd_d;
    logic       tx_ctl_d;

    assign txc_d    = phase_d[1] ^ phase_d[0];
    assign tx_ctl_d = en_now;

    always_comb begin
        if (!en_now)         txd_d = 4'h0;
        else if (phase_d[2]) txd_d = byte_now[7:4];
        else                 txd_d = byte_now[3:0];
    end

    // The pins' registers are their output cells', each an ODDR given one
    // value for both halves of a cycle: a cell named outright is in the
    // I/O cell or the build fails, where a register asked into one by an
    // attribute can be left in the fabric with a warning.
    ODDR #(
        .DDR_CLK_EDGE ("SAME_EDGE")
    ) u_txc_oddr (
        .Q  (o_rgmii_txc),
        .C  (i_tx_clk),
        .CE (1'b1),
        .D1 (txc_d),
        .D2 (txc_d),
        .R  (1'b0),
        .S  (1'b0)
    );

    for (genvar i = 0; i < 4; i++) begin : g_txd
        ODDR #(
            .DDR_CLK_EDGE ("SAME_EDGE")
        ) u_oddr (
            .Q  (o_rgmii_txd[i]),
            .C  (i_tx_clk),
            .CE (1'b1),
            .D1 (txd_d[i]),
            .D2 (txd_d[i]),
            .R  (1'b0),
            .S  (1'b0)
        );
    end

    ODDR #(
        .DDR_CLK_EDGE ("SAME_EDGE")
    ) u_tx_ctl_oddr (
        .Q  (o_rgmii_tx_ctl),
        .C  (i_tx_clk),
        .CE (1'b1),
        .D1 (tx_ctl_d),
        .D2 (tx_ctl_d),
        .R  (1'b0),
        .S  (1'b0)
    );

endmodule
