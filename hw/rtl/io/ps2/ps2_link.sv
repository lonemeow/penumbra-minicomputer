// Penumbra PS/2 port: the link
//
// One PS/2 link, between its two pins and single bytes; the register
// block (ps2) sits above it. Design notes: doc/internals/ps2-port.md.
//
// Both lines are open-drain, and the device clocks every frame in
// either direction. All this end does to the clock is hold it low,
// which makes the device wait: while the port is not in use, and behind
// a received byte until it is taken, so one byte is kept and none lost.
//
// A frame is eleven bits, one to each fall of the clock: a start bit
// (0), the byte from its least significant bit, odd parity, a stop bit
// (1). A bit from the device is taken as the clock falls; a bit to it
// changes there, and the device takes it as the clock rises.
//
// The clock line is low under either end's pull, and only the device's
// is a clock edge. The port's own request for the clock passes the same
// filter as the two lines, so it arrives beside its effect on the pin,
// and a fall counts only while that copy is high (device_edge).
//
// No reset of its own: while i_en is low the port holds the clock low
// and keeps nothing, which is also the state the FPGA configures to.

module ps2_link #(
    parameter int CLK_HZ,                   // i_clk's rate; the integration states it
    parameter int FILTER_NS    = 1_000,     // a level counts once it has stood this long
    parameter int CLAIM_US     = 100,       // the clock held low ahead of a byte to send
    parameter int REQUEST_US   = 5,         // data low before the clock is let go
    parameter int FRAME_GAP_US = 1_000,     // a frame with no clock edge for this long is given up
    parameter int TX_LIMIT_US  = 18_000     // a byte not delivered this long after its start is given up
)(
    input  logic       i_clk,

    input  logic       i_en,          // the port is in use (CTRL.EN)

    // ── Received byte ────────────────────────────────────────
    output logic       o_rx_valid,    // a byte waits to be taken
    output logic [7:0] o_rx_byte,     // the byte; meaningful while o_rx_valid
    output logic       o_rx_err,      // its frame was not well formed; meaningful while o_rx_valid
    input  logic       i_rx_take,     // one cycle: the waiting byte has been taken

    // ── Byte to send ─────────────────────────────────────────
    input  logic       i_tx_start,    // one cycle: send i_tx_byte; ignored while o_tx_busy or !i_en
    input  logic [7:0] i_tx_byte,
    output logic       o_tx_busy,     // a byte is being sent
    output logic       o_tx_err,      // the last byte sent was not delivered; held until the next start

    // ── Pins ─────────────────────────────────────────────────
    output logic       o_ps2_clk,     // the level asked for: 1 lets the line go, 0 pulls it low
    input  logic       i_ps2_clk,     // the level the line carries
    output logic       o_ps2_data,
    input  logic       i_ps2_data
);

    // ── Times, in cycles, rounded up ─────────────────────────
    localparam longint CLK_HZ_L = longint'(CLK_HZ);

    localparam int FILTER_CYCLES    = int'((CLK_HZ_L * FILTER_NS    + 999_999_999) / 1_000_000_000);
    localparam int CLAIM_CYCLES     = int'((CLK_HZ_L * CLAIM_US     + 999_999) / 1_000_000);
    localparam int REQUEST_CYCLES   = int'((CLK_HZ_L * REQUEST_US   + 999_999) / 1_000_000);
    localparam int FRAME_GAP_CYCLES = int'((CLK_HZ_L * FRAME_GAP_US + 999_999) / 1_000_000);
    localparam int TX_LIMIT_CYCLES  = int'((CLK_HZ_L * TX_LIMIT_US  + 999_999) / 1_000_000);

    // A PS/2 clock stands 30 us at a level, half that on a fast device.
    build_check #(
        .COND (FILTER_NS <= 5_000),
        .MSG  ("ps2_link: FILTER_NS is too long for the short half of a PS/2 clock period")
    ) u_check_filter ();

    build_check #(
        .COND (CLAIM_US >= 100),
        .MSG  ("ps2_link: CLAIM_US is under the 100 us a device is owed before a request to send")
    ) u_check_claim ();

    // A device may take 15 ms to start clocking a byte in and 2 ms over
    // it; the interface promises software an end inside 20 ms.
    build_check #(
        .COND (TX_LIMIT_US >= CLAIM_US + REQUEST_US + 17_000 && TX_LIMIT_US < 20_000),
        .MSG  ("ps2_link: TX_LIMIT_US must leave a device 17 ms and end inside the interface's 20 ms")
    ) u_check_tx_limit ();

    localparam int TIMER_MAX = (TX_LIMIT_CYCLES > FRAME_GAP_CYCLES) ? TX_LIMIT_CYCLES
                                                                    : FRAME_GAP_CYCLES;
    localparam int TIMER_W   = (TIMER_MAX > 0) ? $clog2(TIMER_MAX + 1) : 1;

    typedef logic [TIMER_W-1:0] timer_t;
    localparam timer_t T_CLAIM_END   = timer_t'(CLAIM_CYCLES);
    localparam timer_t T_REQUEST_END = timer_t'(CLAIM_CYCLES + REQUEST_CYCLES);
    localparam timer_t T_TX_LIMIT    = timer_t'(TX_LIMIT_CYCLES);
    localparam timer_t T_FRAME_GAP   = timer_t'(FRAME_GAP_CYCLES);

    // A frame's bits, by number.
    typedef logic [3:0] bits_t;
    localparam bits_t BIT_START  = 4'd0;
    localparam bits_t BIT_DATA0  = 4'd1;
    localparam bits_t BIT_DATA7  = 4'd8;
    localparam bits_t BIT_PARITY = 4'd9;
    localparam bits_t BIT_STOP   = 4'd10;

    // ── The lines in ─────────────────────────────────────────
    logic clk_level;        // the clock line, filtered
    logic data_level;       // the data line, with the same delay
    logic own_clk_level;    // o_ps2_clk, with the same delay: 0 where the pull is the port's own

    ps2_line_filter #(
        .FILTER_CYCLES (FILTER_CYCLES)
    ) u_clk_in (
        .i_clk   (i_clk),
        .i_line  (i_ps2_clk),
        .o_level (clk_level)
    );

    ps2_line_filter #(
        .FILTER_CYCLES (FILTER_CYCLES)
    ) u_data_in (
        .i_clk   (i_clk),
        .i_line  (i_ps2_data),
        .o_level (data_level)
    );

    ps2_line_filter #(
        .FILTER_CYCLES (FILTER_CYCLES)
    ) u_own_clk (
        .i_clk   (i_clk),
        .i_line  (o_ps2_clk),
        .o_level (own_clk_level)
    );

    logic clk_level_q;      // clk_level one cycle earlier

    // One cycle for each time the device pulls the clock low. In that
    // cycle data_level is the data line as it stood when the clock fell.
    logic device_edge;
    assign device_edge = clk_level_q && !clk_level && own_clk_level;

    // ── State ────────────────────────────────────────────────
    typedef enum logic [1:0] {
        S_LISTEN,       // the device may send: a frame's bits are taken as it clocks them
        S_TX_CLAIM,     // the clock held low ahead of a byte to send
        S_TX_REQUEST,   // data low as well: the request to send
        S_TX_BITS       // the clock let go, the device clocking the byte in
    } state_t;

    state_t state_q;

    // S_LISTEN: the number of the bit the next device_edge carries, 0
    // with no frame in flight. S_TX_BITS: edges given to the byte so far.
    bits_t  bits_q;

    // S_LISTEN: cycles since the frame in flight had its last bit.
    // Sending: cycles since the send started.
    timer_t timer_q;

    logic       rx_valid_q;     // a byte waits; the device is held off meanwhile

    // The last eleven bits from the device, the newest in [10]. While
    // rx_valid_q is 1 that is one whole frame, bit k in [k], and the
    // byte and its error flag are read straight off it. Nothing clears
    // it: eleven bits push out whatever an unfinished frame left.
    logic [10:0] rx_frame_q;

    // The frame to send, the bit for the data line in [0]. Ones come in
    // above, so the line is let go for the device's acknowledge.
    logic [10:0] tx_frame_q;
    logic        tx_err_q;

    localparam bits_t TX_ACK_EDGE = 4'd10;  // edges before the one the device acknowledges on

    assign o_rx_valid = rx_valid_q;
    assign o_rx_byte  = rx_frame_q[BIT_DATA7:BIT_DATA0];
    assign o_rx_err   = rx_frame_q[BIT_START]
                     || !rx_frame_q[BIT_STOP]
                     || !(^rx_frame_q[BIT_PARITY:BIT_DATA0]);
    assign o_tx_busy  = (state_q != S_LISTEN);
    assign o_tx_err   = tx_err_q;

    always_ff @(posedge i_clk) begin
        clk_level_q <= clk_level;

        if (!i_en) begin
            state_q    <= S_LISTEN;
            bits_q     <= '0;
            timer_q    <= '0;
            rx_valid_q <= 1'b0;
            tx_err_q   <= 1'b0;
        end else begin
            if (i_rx_take)
                rx_valid_q <= 1'b0;

            case (state_q)
                S_LISTEN: begin
                    if (i_tx_start) begin
                        // Whatever of a frame had arrived goes: the
                        // claim makes the device send it again.
                        state_q    <= S_TX_CLAIM;
                        tx_frame_q <= {1'b1, ~^i_tx_byte, i_tx_byte, 1'b0};
                        tx_err_q   <= 1'b0;
                        bits_q     <= '0;
                        timer_q    <= '0;
                    end else if (device_edge && !rx_valid_q) begin
                        timer_q    <= '0;
                        rx_frame_q <= {data_level, rx_frame_q[10:1]};
                        if (bits_q == BIT_STOP) begin
                            bits_q     <= '0;
                            rx_valid_q <= 1'b1;
                        end else begin
                            bits_q <= bits_q + 4'd1;
                        end
                    end else if (bits_q != BIT_START) begin
                        // A frame the device stops clocking is given
                        // up, so its next is taken from the first bit.
                        if (timer_q == T_FRAME_GAP)
                            bits_q <= '0;
                        else
                            timer_q <= timer_q + timer_t'(1);
                    end
                end

                S_TX_CLAIM: begin
                    timer_q <= timer_q + timer_t'(1);
                    if (timer_q == T_CLAIM_END)
                        state_q <= S_TX_REQUEST;
                end

                S_TX_REQUEST: begin
                    timer_q <= timer_q + timer_t'(1);
                    if (timer_q == T_REQUEST_END)
                        state_q <= S_TX_BITS;
                end

                S_TX_BITS: begin
                    timer_q <= timer_q + timer_t'(1);
                    if (device_edge) begin
                        tx_frame_q <= {1'b1, tx_frame_q[10:1]};
                        bits_q     <= bits_q + 4'd1;
                        if (bits_q == TX_ACK_EDGE) begin
                            // The device holds data low on this edge
                            // for a byte it has taken.
                            state_q  <= S_LISTEN;
                            tx_err_q <= data_level;
                            bits_q   <= '0;
                        end
                    end else if (timer_q == T_TX_LIMIT) begin
                        state_q  <= S_LISTEN;
                        tx_err_q <= 1'b1;
                        bits_q   <= '0;
                    end
                end
            endcase
        end
    end

    // ── The lines out ────────────────────────────────────────
    // A byte to send goes ahead of a byte waiting: the clock is let go
    // for the device to clock the one in, then held again for the other.
    logic clk_request, data_request;

    always_comb begin
        if (!i_en)
            clk_request = 1'b0;
        else if (state_q == S_TX_CLAIM || state_q == S_TX_REQUEST)
            clk_request = 1'b0;
        else if (state_q == S_TX_BITS)
            clk_request = 1'b1;
        else
            clk_request = !rx_valid_q;
    end

    assign data_request = (state_q == S_TX_REQUEST || state_q == S_TX_BITS) ? tx_frame_q[0]
                                                                              : 1'b1;

    // Each leaves from a flop at its pin. The clock is held low from
    // configuration on, the port not yet in use.
    /* verilator lint_off PROCASSINIT */
    logic ps2_clk_q  = 1'b0;
    logic ps2_data_q = 1'b1;
    /* verilator lint_on PROCASSINIT */

    always_ff @(posedge i_clk) begin
        ps2_clk_q  <= clk_request;
        ps2_data_q <= data_request;
    end

    assign o_ps2_clk  = ps2_clk_q;
    assign o_ps2_data = ps2_data_q;

endmodule
