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
    output logic [7:0] o_rdata,    // read data (valid same cycle as i_rd)

    // Status
    output logic       o_full,
    output logic       o_empty,
    output logic [LEVEL_BITS-1:0] o_level,  // number of entries currently stored

    // Control
    input  logic       i_flush     // synchronous clear
);

    localparam ADDR_WIDTH = $clog2(DEPTH);
    localparam LEVEL_BITS = ADDR_WIDTH + 1;   // 0..DEPTH

    // Storage
    logic [7:0] mem [DEPTH];

    // Pointers — one extra MSB for full/empty disambiguation
    logic [ADDR_WIDTH:0] wr_ptr, rd_ptr;

    assign o_empty = wr_ptr == rd_ptr;
    assign o_full  = (wr_ptr[ADDR_WIDTH-1:0] == rd_ptr[ADDR_WIDTH-1:0]) &&
                     (wr_ptr[ADDR_WIDTH] != rd_ptr[ADDR_WIDTH]);
    assign o_level = wr_ptr - rd_ptr;
    assign o_rdata = mem[rd_ptr[ADDR_WIDTH-1:0]];

    logic write_valid;
    logic read_valid;

    assign write_valid = i_wr && (!o_full || i_rd);
    assign read_valid  = i_rd && !o_empty;

    always_ff @(posedge i_clk) begin
        if (i_rst || i_flush) begin
            wr_ptr <= '0;
            rd_ptr <= '0;
        end else begin
            if (write_valid) begin
                mem[wr_ptr[ADDR_WIDTH-1:0]] <= i_wdata;
                wr_ptr      <= wr_ptr + 1;
            end
            if (read_valid) begin
                rd_ptr  <= rd_ptr + 1;
            end
        end
    end

endmodule
