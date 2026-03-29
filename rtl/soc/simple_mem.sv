// Penumbra Simple Memory — synchronous SRAM model for simulation
//
// 4K words (16 KB), word-addressed. Loads program.hex at init.
// Synchronous read with 1-cycle latency, synchronous write.
// Provides busy signal for the STALL mechanism.
//
// Byte enables gate per-byte writes for sub-word stores.
// Reads always return the full 32-bit word — byte extraction
// is done by the CPU datapath.
//
// In the real SoC this gets replaced by the cache ↔ bus path.

// verilator lint_off UNUSEDSIGNAL

module simple_mem
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,     // Byte address (bits [13:2] used)
    input  logic [31:0] i_wdata,    // Write data
    input  logic [3:0]  i_byte_en,  // Per-byte write enables (active high)
    input  logic        i_we,       // Write enable (data access)
    input  logic        i_re,       // Read enable (data access)
    output logic [31:0] o_rdata,    // Read data (always valid, 1-cycle latency)
    output logic        o_busy      // Access in progress (for STALL)
);

    localparam MEM_WORDS = 4096;

    logic [31:0] mem [0:MEM_WORDS-1];

    initial begin
        for (int i = 0; i < MEM_WORDS; i++)
            mem[i] = 32'b0;
        $readmemh("program.hex", mem);
    end

    // Word-addressed (drop lower 2 bits)
    logic [11:0] word_addr;
    assign word_addr = i_addr[13:2];

    // ── Synchronous write (per-byte enables) ────────────────
    always_ff @(posedge i_clk) begin
        if (i_we) begin
            if (i_byte_en[0]) mem[word_addr][ 7: 0] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem[word_addr][15: 8] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem[word_addr][23:16] <= i_wdata[23:16];
            if (i_byte_en[3]) mem[word_addr][31:24] <= i_wdata[31:24];
        end
    end

    // ── Synchronous read (1-cycle latency) ─────────────────
    // Read happens every cycle regardless of i_re — the address
    // bus is always driven (PC during fetch, MAR during data).
    // The i_re signal only affects the busy handshake.
    logic [31:0] rdata_reg;
    always_ff @(posedge i_clk) begin
        rdata_reg <= mem[word_addr];
    end
    assign o_rdata = rdata_reg;

    // ── Busy signal ────────────────────────────────────────
    // Models 1-cycle access latency for the STALL micro-op.
    // Busy on the first cycle of a data access, clear on the next.
    logic access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_pending <= 1'b0;
        else if ((i_re || i_we) && !access_pending)
            access_pending <= 1'b1;   // First cycle — busy
        else
            access_pending <= 1'b0;   // Second cycle — done
    end

    assign o_busy = (i_re || i_we) && !access_pending;

endmodule

// verilator lint_on UNUSEDSIGNAL
