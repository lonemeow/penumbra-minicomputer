// FPGA RAM — BRAM-friendly memory for synthesis
//
// Drop-in replacement for simple_mem on FPGA targets.
// Four byte-wide memory banks so Yosys reliably infers block RAM.
// Single-cycle read/write (no configurable latency — BRAM is fast).
// Same bus interface as simple_mem.
//
// Addresses wrap modulo MEM_WORDS for RAM size probing.

module fpga_ram
    import penumbra_pkg::*;
#(
    parameter MEM_WORDS = 65536   // default 256 KB
)(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy
);

    localparam ADDR_BITS = $clog2(MEM_WORDS);

    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    // ── Four byte-wide banks (clean BRAM inference) ─────────
    (* ram_style = "block" *) logic [7:0] mem0 [0:MEM_WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem1 [0:MEM_WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem2 [0:MEM_WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem3 [0:MEM_WORDS-1];

    // ── Synchronous write (per-byte) ────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_we) begin
            if (i_byte_en[0]) mem0[word_addr] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem1[word_addr] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem2[word_addr] <= i_wdata[23:16];
            if (i_byte_en[3]) mem3[word_addr] <= i_wdata[31:24];
        end
    end

    // ── Synchronous read (1-cycle latency) ──────────────────
    logic [31:0] rdata_reg;
    always_ff @(posedge i_clk) begin
        rdata_reg <= {mem3[word_addr], mem2[word_addr],
                      mem1[word_addr], mem0[word_addr]};
    end
    assign o_rdata = o_busy ? 32'hDEAD_BEEF : rdata_reg;

    // ── Busy: 1-cycle read latency (same protocol as simple_mem) ─
    logic access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_pending <= 1'b0;
        else if ((i_re || i_we) && !access_pending)
            access_pending <= 1'b1;
        else
            access_pending <= 1'b0;
    end

    assign o_busy = (i_re || i_we) && !access_pending;

endmodule
