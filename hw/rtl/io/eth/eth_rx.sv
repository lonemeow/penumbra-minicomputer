// Penumbra Ethernet receiver
//
// Takes frames a byte at a time across the seam from the board's PHY
// adapter and stores the ones the interface says to keep: well formed,
// addressed to this station, and with a slot free for them. A frame goes
// into the slot at o_prod as it arrives, and o_prod counts it only once
// it has ended and passed. One that fails leaves no trace, because the bus
// side reads no slot o_prod has not counted: the next frame is written
// over it.
//
// The seam. In a cycle where i_ce is set, i_rxd, i_dv and i_er are one
// byte time: a byte, whether a frame is in progress, and whether the PHY
// found an error in it. The adapter hands over whole bytes, so a frame is
// some of the preamble's 0x55, the start delimiter 0xD5, and then the
// frame and its check sequence until i_dv falls.
//
// Position is kept as an offset into the slot, as the transmitter keeps
// it. The slot's first word holds LEN, which is known only at the frame's
// end, so the two frame bytes that share that word are held back and the
// word is written last, as the frame is committed. The check sequence is
// stored behind the frame like any other bytes: LEN does not count it.
//
// Whether a slot is free is settled as a frame starts, since a slot that
// comes free later has missed the frame's beginning. A frame that finds
// none is still checked, unstored, and counted in o_drop if it would have
// been received.

module eth_rx #(
    parameter int SLOTS      = 32,    // slots in the receive ring, a power of two
    parameter int SLOT_BYTES = 2048   // bytes in a slot, a power of two
)(
    input  logic        i_clk,        // the receive clock
    input  logic        i_rst,

    // ── Control, in this clock's domain ──────────────────────
    input  logic        i_enable,     // CTRL.RX_EN
    input  logic        i_promisc,    // CTRL.PROMISC
    input  logic        i_allmulti,   // CTRL.ALLMULTI
    input  logic [47:0] i_addr,       // the station address, its first byte in [7:0]
    input  logic [15:0] i_cons,       // RX_CONS: frames software has released
    output logic [15:0] o_prod,       // RX_PROD: frames stored
    output logic        o_drop,       // one cycle: a frame was lost to a full ring

    // ── The receive slots' write port ────────────────────────
    // A word address across the whole ring. The word is written on the
    // edge that ends a cycle with o_mem_we set.
    output logic        o_mem_we,
    output logic [$clog2(SLOTS * SLOT_BYTES / 4)-1:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,

    // ── The seam ─────────────────────────────────────────────
    input  logic        i_ce,         // a byte time is presented this cycle
    input  logic [7:0]  i_rxd,
    input  logic        i_dv,         // a frame is in progress
    input  logic        i_er          // the PHY found an error in it
);

    localparam int MEM_ADDR_W = $clog2(SLOTS * SLOT_BYTES / 4);

    // A slot is LEN in its first two bytes and the frame from the third.
    localparam int LEN_BYTES = 2;
    localparam int POS_W     = $clog2(SLOT_BYTES);
    localparam int FCS_BYTES = 4;

    // The shortest and longest frames, without their check sequences, and
    // the slot offsets the position stands at behind each one's check
    // sequence.
    localparam int MIN_FRAME   = 60;
    localparam int MAX_FRAME   = 1518;
    localparam int MIN_END_POS = LEN_BYTES + MIN_FRAME + FCS_BYTES;
    localparam int MAX_END_POS = LEN_BYTES + MAX_FRAME + FCS_BYTES;

    // What the remainder is after a frame and its own check sequence.
    localparam logic [31:0] FCS_RESIDUE = 32'hDEBB_20E3;

    build_check #(
        .COND (SLOTS >= 1 && (SLOTS & (SLOTS - 1)) == 0),
        .MSG  ("eth_rx: SLOTS is not a power of two")
    ) u_check_slots ();

    build_check #(
        .COND (SLOT_BYTES >= MAX_END_POS && (SLOT_BYTES & (SLOT_BYTES - 1)) == 0),
        .MSG  ("eth_rx: SLOT_BYTES is not a power of two that holds the longest frame")
    ) u_check_slot_bytes ();

    typedef enum logic [2:0] {
        S_HUNT,         // between frames, or in a preamble: look for the delimiter
        S_DATA,         // the frame's bytes
        S_SKIP,         // a frame not to be had: wait for it to end
        S_COMMIT,       // write the slot's first word
        S_COUNT         // count the frame
    } state_t;

    state_t           state_q;
    logic [15:0]      prod_q;
    logic [POS_W-1:0] pos_q;        // the slot offset the next byte goes to
    logic [23:0]      acc_q;        // the bytes of the word being gathered, newest at the top
    logic [15:0]      first_q;      // the frame's first two bytes, which share LEN's word
    logic             store_q;      // the frame has a slot, and is being written to it

    assign o_prod = prod_q;

    // ── The byte time ────────────────────────────────────────
    logic frame_start;      // the start delimiter of a frame to be taken
    logic take_byte;        // a byte of the frame goes into the slot
    logic frame_done;       // the frame in hand has ended

    assign frame_start = i_ce && (state_q == S_HUNT) && i_dv && (i_rxd == 8'hD5) &&
                         i_enable;
    assign take_byte   = i_ce && (state_q == S_DATA) && i_dv && !i_er &&
                         (pos_q != POS_W'(MAX_END_POS));
    assign frame_done  = i_ce && (state_q == S_DATA) && !i_dv;

    logic word_full;        // this byte is the last of its word
    logic in_first_word;    // and the word is the slot's first
    assign word_full     = (pos_q[1:0] == 2'd3);
    assign in_first_word = (pos_q[POS_W-1:2] == '0);

    logic ring_full;
    assign ring_full = (prod_q - i_cons == 16'(SLOTS));

    // ── Address filter ───────────────────────────────────────
    // The destination is the frame's first six bytes, at slot offsets
    // LEN_BYTES to LEN_BYTES + 5. station_in_slot is the station address
    // as it would lie there, so the byte to compare is picked by the
    // offset.
    logic [63:0] station_in_slot;
    logic [7:0]  station_byte;      // the station address's byte for pos_q
    logic        in_dest;           // pos_q is within the destination
    logic        at_dest_first;     // and at its first byte

    assign station_in_slot = {i_addr, 16'h0000};
    assign station_byte    = station_in_slot[8 * pos_q[2:0] +: 8];
    assign in_dest         = (pos_q[POS_W-1:3] == '0);
    assign at_dest_first   = in_dest && (pos_q[2:0] == 3'(LEN_BYTES));

    logic match_q;      // every destination byte so far is the station's
    logic bcast_q;      // every one so far is 0xFF
    logic group_q;      // the destination is a group address, as 802.3 calls one
                        //   with its first bit set: multicast or broadcast
    logic accept;       // the destination passes; settled once all six bytes are in

    // match_q and bcast_q start over at the destination's first byte and
    // narrow with each one after it. group_q is that first byte's low bit.
    always_ff @(posedge i_clk) begin
        if (take_byte && in_dest) begin
            match_q <= (at_dest_first || match_q) && i_rxd == station_byte;
            bcast_q <= (at_dest_first || bcast_q) && i_rxd == 8'hFF;
            group_q <= at_dest_first ? i_rxd[0] : group_q;
        end
    end

    assign accept = i_promisc || match_q || bcast_q || (i_allmulti && group_q);

    // ── Frame check sequence ─────────────────────────────────
    logic [31:0] crc;

    eth_crc32 u_crc (
        .i_clk   (i_clk),
        .i_init  (state_q == S_HUNT),
        .i_valid (take_byte),
        .i_data  (i_rxd),
        .o_crc   (crc)
    );

    // ── The verdict ──────────────────────────────────────────
    // Taken as the frame ends. Too long a frame or one the PHY flagged
    // never gets here.
    logic received;         // well formed and addressed to this station
    assign received = (pos_q >= POS_W'(MIN_END_POS)) && (crc == FCS_RESIDUE) && accept;

    // ── Sequencing ───────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q  <= S_HUNT;
            prod_q   <= '0;
            o_mem_we <= 1'b0;
            o_drop   <= 1'b0;
        end else begin
            o_mem_we <= 1'b0;
            o_drop   <= 1'b0;

            unique case (state_q)
                // Preamble bytes pass. Anything else with a frame in
                // progress is no frame to take up.
                S_HUNT: begin
                    if (frame_start) begin
                        pos_q   <= POS_W'(LEN_BYTES);
                        store_q <= !ring_full;
                        state_q <= S_DATA;
                    end else if (i_ce && i_dv && (i_rxd != 8'h55)) begin
                        state_q <= S_SKIP;
                    end
                end

                S_DATA: begin
                    if (take_byte) begin
                        acc_q <= {i_rxd, acc_q[23:8]};
                        pos_q <= pos_q + POS_W'(1);
                        if (word_full && in_first_word) begin
                            first_q <= {i_rxd, acc_q[23:16]};
                        end else if (word_full && store_q) begin
                            o_mem_we    <= 1'b1;
                            o_mem_addr  <= MEM_ADDR_W'({prod_q, pos_q[POS_W-1:2]});
                            o_mem_wdata <= {i_rxd, acc_q};
                        end
                    end else if (frame_done) begin
                        if (received && store_q) begin
                            state_q <= S_COMMIT;
                        end else begin
                            o_drop  <= received;
                            state_q <= S_HUNT;
                        end
                    end else if (i_ce) begin
                        state_q <= S_SKIP;
                    end
                end

                S_SKIP: begin
                    if (i_ce && !i_dv)
                        state_q <= S_HUNT;
                end

                // The position stands behind the check sequence, so the
                // frame's length is the offset less what lies either side
                // of it.
                S_COMMIT: begin
                    o_mem_we    <= 1'b1;
                    o_mem_addr  <= MEM_ADDR_W'({prod_q, {(POS_W - 2){1'b0}}});
                    o_mem_wdata <= {first_q, 16'(pos_q - POS_W'(LEN_BYTES + FCS_BYTES))};
                    state_q     <= S_COUNT;
                end

                // A cycle behind the write, so the count never names a slot
                // whose first word is not yet in it.
                S_COUNT: begin
                    prod_q  <= prod_q + 16'd1;
                    state_q <= S_HUNT;
                end

                default: state_q <= S_HUNT;
            endcase
        end
    end

endmodule
