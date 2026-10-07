// Penumbra Ethernet adapter slot memory
//
// One ring's slots: a block RAM of words, written on one clock and read on
// another. The adapter holds two. The transmit ring's is written from the
// bus, with a write enable for each byte lane, and read by the transmitter.
// The receive ring's is written by the receiver a word at a time and read
// from the bus. Frame bytes cross between the clocks here and nowhere else.
//
// The two ports are independent, with nothing between them: the ring's
// counts are what keep a slot's reader away until its writer is done.
// Contents are undefined at power-up, as a slot's are until it is counted.
//
// Storage is four byte-wide banks. That makes the byte lanes plain
// per-bank write enables, and it is the shape synthesis reliably infers
// block RAM from.

module eth_slot_ram #(
    parameter int WORDS = 4096      // depth in 32-bit words
)(
    // ── Write port ───────────────────────────────────────────
    input  logic                     i_wclk,
    input  logic [3:0]               i_we,      // one enable for each byte lane
    input  logic [$clog2(WORDS)-1:0] i_waddr,
    input  logic [31:0]              i_wdata,

    // ── Read port ────────────────────────────────────────────
    // o_rdata is the word at the address of the cycle before.
    input  logic                     i_rclk,
    input  logic [$clog2(WORDS)-1:0] i_raddr,
    output logic [31:0]              o_rdata
);

    (* ram_style = "block" *) logic [7:0] mem0 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem1 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem2 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem3 [0:WORDS-1];

    always_ff @(posedge i_wclk) begin
        if (i_we[0]) mem0[i_waddr] <= i_wdata[ 7: 0];
        if (i_we[1]) mem1[i_waddr] <= i_wdata[15: 8];
        if (i_we[2]) mem2[i_waddr] <= i_wdata[23:16];
        if (i_we[3]) mem3[i_waddr] <= i_wdata[31:24];
    end

    logic [31:0] rdata_q;

    always_ff @(posedge i_rclk)
        rdata_q <= {mem3[i_raddr], mem2[i_raddr], mem1[i_raddr], mem0[i_raddr]};

    assign o_rdata = rdata_q;

endmodule
