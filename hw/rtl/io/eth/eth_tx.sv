// Penumbra Ethernet transmitter
//
// Sends the frames queued in the transmit ring, in order, a byte at a time
// across the seam to the board's PHY adapter. For each frame it reads the
// slot's LEN, then sends the preamble, the slot's bytes, zeros up to the
// shortest legal frame where it is shorter, and the frame check sequence,
// and counts the frame in o_cons once its last byte has been taken. A slot
// whose LEN is zero or more than a slot holds sends nothing and is counted
// the same way.
//
// The seam. o_txd and o_tx_en are one byte time's worth: the byte, and
// whether a frame is in progress. The adapter takes them in a cycle where
// i_ce is set, and the transmitter puts up the next ones on that cycle's
// edge. Nothing here knows the link's speed: at 1000 Mbit/s the enable is
// every cycle, a slower link spaces it out, and the gap between frames is
// counted in enables.
//
// Position is kept as an offset into the slot, the unit the memory is
// addressed in, so a byte's word and its place in the word are the
// offset's own bits and nothing converts between a frame's bytes and a
// slot's.
//
// With i_enable clear, the frame on the wire is finished and the rest of
// the queue discarded: o_cons steps up to i_prod without sending. It moves
// one count a cycle there as everywhere, so that it can cross to the bus
// clock as a gray code.

module eth_tx #(
    parameter int SLOTS      = 8,     // slots in the transmit ring, a power of two
    parameter int SLOT_BYTES = 2048   // bytes in a slot, a power of two
)(
    input  logic        i_clk,        // the transmit clock
    input  logic        i_rst,

    // ── Control, in this clock's domain ──────────────────────
    input  logic        i_enable,     // CTRL.TX_EN
    input  logic [15:0] i_prod,       // TX_PROD: frames software has queued
    output logic [15:0] o_cons,       // TX_CONS: frames finished with

    // ── The transmit slots' read port ────────────────────────
    // A word address across the whole ring, combinational from i_ce.
    // i_mem_rdata is the word at the address of the cycle before.
    output logic [$clog2(SLOTS * SLOT_BYTES / 4)-1:0] o_mem_addr,
    input  logic [31:0] i_mem_rdata,

    // ── The seam ─────────────────────────────────────────────
    input  logic        i_ce,         // the adapter takes a byte this cycle
    output logic [7:0]  o_txd,
    output logic        o_tx_en       // a frame is in progress
);

    localparam int MEM_ADDR_W = $clog2(SLOTS * SLOT_BYTES / 4);

    // A slot is LEN in its first two bytes and the frame from the third.
    localparam int LEN_BYTES = 2;
    localparam int MAX_LEN   = SLOT_BYTES - LEN_BYTES;
    localparam int POS_W     = $clog2(SLOT_BYTES);

    // The shortest frame, without its check sequence, and the slot offset
    // its last byte falls at.
    localparam int MIN_FRAME     = 60;
    localparam int MIN_LAST_POS  = LEN_BYTES + MIN_FRAME - 1;

    localparam int PREAMBLE_BYTES = 8;    // seven of 0x55 and the start delimiter
    localparam int FCS_BYTES      = 4;
    localparam int GAP_BYTES      = 12;   // byte times the wire idles between frames
    localparam int COUNT_W        = $clog2(GAP_BYTES);

    build_check #(
        .COND (SLOTS >= 1 && (SLOTS & (SLOTS - 1)) == 0),
        .MSG  ("eth_tx: SLOTS is not a power of two")
    ) u_check_slots ();

    build_check #(
        .COND (SLOT_BYTES >= 2048 && (SLOT_BYTES & (SLOT_BYTES - 1)) == 0),
        .MSG  ("eth_tx: SLOT_BYTES is not a power of two of at least 2048")
    ) u_check_slot_bytes ();

    typedef enum logic [2:0] {
        S_IDLE,         // nothing in hand: look at the ring
        S_LEN,          // the slot's first word is at the memory's output
        S_PREAMBLE,
        S_DATA,         // the slot's bytes
        S_PAD,          // zeros up to MIN_FRAME
        S_FCS,
        S_END           // the last byte is up: the wire goes idle behind it
    } state_t;

    state_t             state_q;
    logic [15:0]        cons_q;
    logic [POS_W-1:0]   left_q;     // bytes of the slot's frame still to put up
    logic [COUNT_W-1:0] count_q;    // bytes into the preamble or the check sequence
    logic [COUNT_W-1:0] gap_q;      // byte times the wire still has to idle

    assign o_cons = cons_q;

    // ── The slot's first word ────────────────────────────────
    logic [15:0] slot_len;
    logic        slot_len_ok;
    assign slot_len    = i_mem_rdata[15:0];
    assign slot_len_ok = (slot_len != '0) && (slot_len <= 16'(MAX_LEN));

    // ── Where in the slot ────────────────────────────────────
    // pos_q is the slot offset of the next byte to put up. Its low two bits
    // are that byte's lane in its word, lane 0 the low byte, and the rest
    // is the word. A slot's frame starts LEN_BYTES in. Between frames the
    // position rests at the slot's start, on the word LEN is in.
    //
    // The memory's read is registered, so it is asked for the word the
    // position will be in after the coming edge, pos_d. Its output is then
    // always the word pos_q is in, even with an enable every cycle.
    logic [POS_W-1:0] pos_q;
    logic [POS_W-1:0] pos_d;

    logic frame_start;      // a slot with a good LEN is taken up
    logic body_byte;        // a byte of the slot or of the padding goes up
    logic frame_end;        // the wire goes idle behind a frame

    assign frame_start = (state_q == S_LEN) && slot_len_ok;
    assign body_byte   = i_ce && (state_q == S_DATA || state_q == S_PAD);
    assign frame_end   = i_ce && (state_q == S_END);

    always_comb begin
        pos_d = pos_q;
        if (frame_start)
            pos_d = POS_W'(LEN_BYTES);
        else if (frame_end)
            pos_d = '0;
        else if (body_byte)
            pos_d = pos_q + 1;
    end

    assign o_mem_addr = MEM_ADDR_W'({cons_q, pos_d[POS_W-1:2]});

    logic [7:0] slot_byte;      // the slot's byte at pos_q
    assign slot_byte = i_mem_rdata[8 * pos_q[1:0] +: 8];

    // ── Frame check sequence ─────────────────────────────────
    // Over the slot's bytes and the padding. The remainder's complement
    // goes out, low byte first.
    logic [7:0]  crc_data;
    logic [31:0] crc;
    logic [31:0] fcs;

    assign crc_data = (state_q == S_DATA) ? slot_byte : 8'h00;
    assign fcs      = ~crc;

    eth_crc32 u_crc (
        .i_clk   (i_clk),
        .i_init  (state_q == S_PREAMBLE),
        .i_valid (body_byte),
        .i_data  (crc_data),
        .o_crc   (crc)
    );

    // ── Sequencing ───────────────────────────────────────────
    logic queued;
    assign queued = (cons_q != i_prod);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q <= S_IDLE;
            cons_q  <= '0;
            pos_q   <= '0;
            gap_q   <= '0;
            o_tx_en <= 1'b0;
            o_txd   <= '0;
        end else begin
            pos_q <= pos_d;

            if (i_ce && gap_q != '0)
                gap_q <= gap_q - COUNT_W'(1);

            unique case (state_q)
                // The memory is asked for the first word of the slot at
                // o_cons throughout, so a cycle here has it ready.
                S_IDLE: begin
                    if (queued && i_enable)
                        state_q <= S_LEN;
                    else if (queued)
                        cons_q <= cons_q + 16'd1;
                end

                S_LEN: begin
                    if (slot_len_ok) begin
                        left_q  <= slot_len[POS_W-1:0];
                        count_q <= '0;
                        state_q <= S_PREAMBLE;
                    end else begin
                        cons_q  <= cons_q + 16'd1;
                        state_q <= S_IDLE;
                    end
                end

                // Waits out what is left of the gap behind the last frame.
                S_PREAMBLE: begin
                    if (i_ce && gap_q == '0) begin
                        o_tx_en <= 1'b1;
                        o_txd   <= (count_q == COUNT_W'(PREAMBLE_BYTES - 1)) ? 8'hD5 : 8'h55;
                        count_q <= count_q + COUNT_W'(1);
                        if (count_q == COUNT_W'(PREAMBLE_BYTES - 1)) begin
                            count_q <= '0;
                            state_q <= S_DATA;
                        end
                    end
                end

                S_DATA: begin
                    if (i_ce) begin
                        o_txd  <= slot_byte;
                        left_q <= left_q - POS_W'(1);
                        if (left_q == POS_W'(1))
                            state_q <= (pos_q < POS_W'(MIN_LAST_POS)) ? S_PAD : S_FCS;
                    end
                end

                S_PAD: begin
                    if (i_ce) begin
                        o_txd <= 8'h00;
                        if (pos_q == POS_W'(MIN_LAST_POS))
                            state_q <= S_FCS;
                    end
                end

                S_FCS: begin
                    if (i_ce) begin
                        o_txd   <= fcs[8 * count_q[1:0] +: 8];
                        count_q <= count_q + COUNT_W'(1);
                        if (count_q == COUNT_W'(FCS_BYTES - 1))
                            state_q <= S_END;
                    end
                end

                // The adapter takes the last byte on this enable. The byte
                // time that starts here is the first of the gap.
                S_END: begin
                    if (i_ce) begin
                        o_tx_en <= 1'b0;
                        gap_q   <= COUNT_W'(GAP_BYTES - 1);
                        cons_q  <= cons_q + 16'd1;
                        state_q <= S_IDLE;
                    end
                end

                default: state_q <= S_IDLE;
            endcase
        end
    end

endmodule
