// SPI FIFO — parameterized synchronous FIFO for SPI TX/RX paths
//
// Simple circular buffer: power-of-2 depth, 8-bit data width,
// separate read/write ports, synchronous flush.
//
// No frame boundary tracking (unlike pnic_fifo) — SPI transfers
// are count-driven, not packet-delimited.
//
// Full/empty use the classic "extra MSB" trick: pointers are
// (ADDR_WIDTH+1) bits wide.  When the low ADDR_WIDTH bits match,
// the MSBs distinguish full (differ) from empty (same).
//
// The storage is block RAM, and a pushed entry becomes poppable one
// cycle after its push: o_empty compares against the write pointer
// as it stood a cycle earlier. The RAM reads the next head every
// cycle, and by the time an entry is poppable that read has seen it
// written, so a pop's o_rdata never needs a path around the RAM.
// o_full and o_level count an entry from its push, so the producer
// can never overfill; a consumer that reads o_level and then pops is
// a bus access later, when every entry counted is poppable.

module spi_fifo
    import penumbra_pkg::*;
#(
    parameter DEPTH = 512   // must be power of 2
)(
    input  logic       i_clk,
    input  logic       i_rst,

    // Write port
    input  logic       i_wr,       // write enable (push)
    input  logic [7:0] i_wdata,    // write data

    // Read port
    input  logic       i_rd,       // read enable (pop)
    output logic [7:0] o_rdata,    // head entry, valid whenever !o_empty

    // Status
    output logic       o_full,
    output logic       o_empty,    // no entry poppable yet
    output logic [LEVEL_BITS-1:0] o_level,  // entries pushed and not yet popped

    // Control
    input  logic       i_flush     // synchronous clear
);

    localparam ADDR_WIDTH = $clog2(DEPTH);
    localparam LEVEL_BITS = ADDR_WIDTH + 1;   // 0..DEPTH

    // Block RAM at every depth: the SPI's 512-entry FIFOs need it, one
    // form keeps every instance mapped alike under both synthesizers, and
    // on the ECP5, whose fmax is set by fabric routing, a memory in block
    // RAM is one the fabric never has to route. Vivado notes that the
    // UART's 16-entry FIFOs would fit LUT RAM better (Synth 8-7082); that
    // would save one 18 Kb block each.
    (* ram_style = "block" *) logic [7:0] mem [DEPTH];

    // Pointers — one extra MSB for full/empty disambiguation
    logic [ADDR_WIDTH:0] wr_ptr_q, rd_ptr_q;
    logic [ADDR_WIDTH:0] wr_ptr_d, rd_ptr_d;
    logic [ADDR_WIDTH:0] wr_ptr_seen_q;   // wr_ptr_q one cycle late: the pop side's view

    assign o_empty = wr_ptr_seen_q == rd_ptr_q;
    assign o_full  = (wr_ptr_q[ADDR_WIDTH-1:0] == rd_ptr_q[ADDR_WIDTH-1:0]) &&
                     (wr_ptr_q[ADDR_WIDTH] != rd_ptr_q[ADDR_WIDTH]);
    assign o_level = wr_ptr_q - rd_ptr_q;

    logic write_valid;
    logic read_valid;
    logic write_en;     // write_valid that is not cleared by a reset or flush

    assign write_valid = i_wr && (!o_full || i_rd);
    assign read_valid  = i_rd && !o_empty;
    assign write_en    = write_valid && !(i_rst || i_flush);

    always_comb begin
        if (i_rst || i_flush) begin
            wr_ptr_d = '0;
            rd_ptr_d = '0;
        end else begin
            wr_ptr_d = write_valid ? wr_ptr_q + 1'b1 : wr_ptr_q;
            rd_ptr_d = read_valid  ? rd_ptr_q + 1'b1 : rd_ptr_q;
        end
    end

    always_ff @(posedge i_clk) begin
        wr_ptr_q      <= wr_ptr_d;
        rd_ptr_q      <= rd_ptr_d;
        wr_ptr_seen_q <= (i_rst || i_flush) ? '0 : wr_ptr_q;
    end

    always_ff @(posedge i_clk) begin
        if (write_en)
            mem[wr_ptr_q[ADDR_WIDTH-1:0]] <= i_wdata;
    end

    always_ff @(posedge i_clk) begin
        o_rdata <= mem[rd_ptr_d[ADDR_WIDTH-1:0]];
    end

endmodule
