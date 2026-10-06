// Penumbra Ethernet PHY management master
//
// Sends one management frame (IEEE 802.3 clause 22) to a PHY for each
// start it is given: 32 ones, then the start pattern, the operation, the
// PHY address and the register address, a turnaround, and 16 bits of data,
// which a write sends and a read takes. The Ethernet adapter's register
// block turns a write of its MDIO register into one start, and o_busy is
// that register's BUSY.
//
// Timing. MDC is the bus clock divided, never above MDC_HZ, and runs only
// while a frame does. A bit time is one MDC period, low half first: the
// data pin takes the bit's value as MDC falls, and the PHY samples it as
// MDC rises, half a period from either change. A PHY answers a read the
// other way about: it moves the pin after a rising edge, by up to
// PHY_VALID_NS, and the master takes each bit just ahead of the next.
//
// A frame is followed by one more bit time with the pin released. A PHY
// that has answered a read lets go of the pin on the last bit's rising
// edge and may take PHY_VALID_NS over it, longer than the half period
// that bit has left, so without the wait a frame started at once could
// drive its first bit against the PHY.
//
// No reset. A frame in flight is the PHY's as much as the machine's, and a
// PHY does not stop for the machine's reset: with a write cut short, it
// would take the next frame's opening bits as the rest of its data. So a
// frame runs to its end. Idle is the state the FPGA configures to, and the
// register block, which does reset, starts nothing while it is held.

module eth_mdio #(
    parameter int CLK_HZ       = 60_000_000,
    parameter int MDC_HZ       = 2_500_000,  // a ceiling: MDC never runs above it
    parameter int PHY_VALID_NS = 300,        // MDC's rise to a PHY's answer, at most
    parameter int PIN_NS       = 20          // out of MDC's pin and in at the data pin's, at most
)(
    input  logic        i_clk,

    // ── Transaction ──────────────────────────────────────────
    input  logic        i_start,    // begin a frame; ignored while o_busy
    input  logic        i_write,    // the frame writes i_wdata; else it reads
    input  logic [4:0]  i_phy_addr, // the PHY's address on the management bus
    input  logic [4:0]  i_reg_addr, // the register's, within the PHY
    input  logic [15:0] i_wdata,
    output logic        o_busy,     // a frame is in flight
    output logic        o_done,     // one cycle, as a frame ends
    output logic [15:0] o_rdata,    // a read's answer, from its o_done on

    // ── Pins ─────────────────────────────────────────────────
    // o_mdio goes onto the data pin while o_mdio_oe is set. The pin is
    // released otherwise, and the board's pull-up holds it high. i_mdio
    // is what the pin carries.
    output logic        o_mdc,
    output logic        o_mdio,
    output logic        o_mdio_oe,
    input  logic        i_mdio
);

    // Clocks in half an MDC period, rounded up.
    localparam int HALF_CYCLES = (CLK_HZ + 2 * MDC_HZ - 1) / (2 * MDC_HZ);
    localparam int HALF_W      = $clog2(HALF_CYCLES);

    // What follows a frame's preamble, in the order it is sent.
    localparam int DATA_BITS = 16;

    typedef struct packed {
        logic [1:0]           start;
        logic [1:0]           op;
        logic [4:0]           phy_addr;
        logic [4:0]           reg_addr;
        logic [1:0]           turnaround;
        logic [DATA_BITS-1:0] data;
    } frame_word_t;

    localparam logic [1:0] START    = 2'b01;
    localparam logic [1:0] OP_WRITE = 2'b01;
    localparam logic [1:0] OP_READ  = 2'b10;
    localparam logic [1:0] TA_WRITE = 2'b10;

    // A frame is its preamble and that word, the data last in it. One
    // bit time more follows the frame with the pin released and MDC at
    // rest.
    localparam int PREAMBLE_BITS = 32;
    localparam int WORD_BITS     = $bits(frame_word_t);
    localparam int FRAME_BITS    = PREAMBLE_BITS + WORD_BITS;
    localparam int BIT_TIMES     = FRAME_BITS + 1;
    localparam int COUNT_W       = $clog2(BIT_TIMES + 1);
    localparam int WORD_IDX_W    = $clog2(WORD_BITS);

    // A read takes the pin in the second cycle of MDC's high half. The
    // pin's level takes two cycles to come through the synchroniser, and
    // MDC leaves through a register, a cycle behind the state that counts
    // its halves. So what is taken there is the pin as it stood one cycle
    // before MDC rose on it, a whole period less that cycle after the
    // rise the PHY answered from. The PHY's answer has to be there by
    // then, and the pins' own delays come out of the same time.
    localparam int SAMPLE_CYCLE = 1;

    build_check #(
        .COND (HALF_CYCLES > SAMPLE_CYCLE),
        .MSG  ("eth_mdio: CLK_HZ leaves half an MDC period too short to take a read's bit in")
    ) u_check_half ();

    build_check #(
        .COND (longint'(2 * HALF_CYCLES - 1) * 64'd1_000_000_000
               >= (longint'(PHY_VALID_NS) + longint'(PIN_NS)) * longint'(CLK_HZ)),
        .MSG  ("eth_mdio: a PHY's answer is not yet on the pin where a read takes it; lower MDC_HZ")
    ) u_check_valid ();

    // ── Pin in ───────────────────────────────────────────────
    // ASYNC_REG on both synchroniser flops: Vivado places the pair
    // together and its clock-crossing check recognises it.
    (* ASYNC_REG = "TRUE" *) logic mdio_s1_q, mdio_s2_q;

    always_ff @(posedge i_clk) begin
        {mdio_s2_q, mdio_s1_q} <= {mdio_s1_q, i_mdio};
    end

    // ── The frame in flight ──────────────────────────────────
    // Bit times left, the current one among them. Zero is idle, which is
    // what the FPGA configures the count to.
    /* verilator lint_off PROCASSINIT */
    logic [COUNT_W-1:0] bits_left_q = '0;
    /* verilator lint_on PROCASSINIT */

    logic [HALF_W-1:0]  half_q;     // cycles into the current half of MDC
    logic               mdc_q;      // the half: 0 the low one, 1 the high

    // What the frame carries, held from its start. A read shifts its
    // answer into data_q, which a write leaves as it was given.
    logic               write_q;
    logic [4:0]         phy_addr_q;
    logic [4:0]         reg_addr_q;
    logic [15:0]        data_q;

    logic busy;
    assign busy    = (bits_left_q != '0);
    assign o_busy  = busy;
    assign o_rdata = data_q;

    // The bit time under way: 0 to FRAME_BITS - 1 through the frame, then
    // FRAME_BITS for the released one. Past that while idle.
    logic [COUNT_W-1:0] bit_idx;
    assign bit_idx = COUNT_W'(BIT_TIMES) - bits_left_q;

    logic half_end, bit_end, sample;
    assign half_end = (half_q == HALF_W'(HALF_CYCLES - 1));
    assign bit_end  = mdc_q && half_end;
    assign sample   = mdc_q && (half_q == HALF_W'(SAMPLE_CYCLE));

    logic in_data;
    assign in_data = (bit_idx >= COUNT_W'(FRAME_BITS - DATA_BITS)) &&
                     (bit_idx <  COUNT_W'(FRAME_BITS));

    always_ff @(posedge i_clk) begin
        o_done <= 1'b0;

        if (!busy) begin
            half_q <= '0;
            mdc_q  <= 1'b0;
            if (i_start) begin
                bits_left_q <= COUNT_W'(BIT_TIMES);
                write_q     <= i_write;
                phy_addr_q  <= i_phy_addr;
                reg_addr_q  <= i_reg_addr;
                data_q      <= i_wdata;
            end
        end else begin
            if (half_end) begin
                half_q <= '0;
                mdc_q  <= !mdc_q;
            end else begin
                half_q <= half_q + HALF_W'(1);
            end

            if (bit_end) begin
                bits_left_q <= bits_left_q - COUNT_W'(1);
                o_done      <= (bits_left_q == COUNT_W'(1));
            end

            if (sample && in_data && !write_q)
                data_q <= {data_q[DATA_BITS-2:0], mdio_s2_q};
        end
    end

    // ── The frame's word ─────────────────────────────────────
    // The word after the preamble, and which of its bits the master
    // drives onto the pin. A read's turnaround and data are the PHY's.
    // The master lets go from the turnaround's first bit, so that the PHY
    // puts its second onto a free pin.
    frame_word_t frame_word;
    frame_word_t drive_mask;

    assign frame_word = '{
        start:      START,
        op:         write_q ? OP_WRITE : OP_READ,
        phy_addr:   phy_addr_q,
        reg_addr:   reg_addr_q,
        turnaround: TA_WRITE,
        data:       data_q
    };

    assign drive_mask = '{
        start:      '1,
        op:         '1,
        phy_addr:   '1,
        reg_addr:   '1,
        turnaround: {2{write_q}},
        data:       {DATA_BITS{write_q}}
    };

    // ── Pins out ─────────────────────────────────────────────
    logic [WORD_IDX_W-1:0] word_idx;    // the bit time's place in the word
    assign word_idx = WORD_IDX_W'(bit_idx - COUNT_W'(PREAMBLE_BITS));

    logic pin_drive, pin_value;

    always_comb begin
        if (bit_idx < COUNT_W'(PREAMBLE_BITS)) begin
            pin_drive = 1'b1;
            pin_value = 1'b1;
        end else if (bit_idx < COUNT_W'(FRAME_BITS)) begin
            pin_drive = drive_mask[WORD_IDX_W'(WORD_BITS - 1) - word_idx];
            pin_value = frame_word[WORD_IDX_W'(WORD_BITS - 1) - word_idx];
        end else begin
            pin_drive = 1'b0;
            pin_value = 1'b1;
        end
    end

    // Registered, so that each leaves from a flop at its pin and all three
    // keep the alignment the counts give them. MDC runs through the frame
    // and not through the bit time after it.
    always_ff @(posedge i_clk) begin
        o_mdc     <= mdc_q && (bit_idx < COUNT_W'(FRAME_BITS));
        o_mdio    <= pin_value;
        o_mdio_oe <= busy && pin_drive;
    end

endmodule
