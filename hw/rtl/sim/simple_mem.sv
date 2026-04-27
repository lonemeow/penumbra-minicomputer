// Penumbra Simple Memory — synchronous SRAM model for simulation
//
// Parameterizable size, word-addressed. Initialized to zero (matches
// real hardware where RAM content is undefined at power-on).
// Configurable read and write latency via parameters.
// Provides busy signal for the STALL mechanism.
//
// Addresses wrap modulo MEM_WORDS so software can probe RAM size
// by writing/reading at power-of-two boundaries.
//
// Byte enables gate per-byte writes for sub-word stores.
// Reads always return the full 32-bit word — byte extraction
// is done by the CPU datapath.
//
// Read data is poisoned (0xDEAD_BEEF) while busy is asserted to
// catch any logic that samples data before the access completes.
//
// In the real SoC this gets replaced by the cache ↔ bus path.

// verilator lint_off UNUSEDSIGNAL

module simple_mem
    import penumbra_pkg::*;
#(
    parameter MEM_WORDS      = 4 * 1024 * 1024,  // Default 4M words = 16 MB
    parameter READ_LATENCY   = 6,                // Read cycles (models SDRAM @ 25 MHz)
    parameter WRITE_LATENCY  = 3                  // Write cycles (models SDRAM @ 25 MHz)
)
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,     // Byte address (wraps modulo MEM_WORDS)
    input  logic [31:0] i_wdata,    // Write data
    input  logic [3:0]  i_byte_en,  // Per-byte write enables (active high)
    input  logic        i_we,       // Write enable (data access)
    input  logic        i_re,       // Read enable (data access)
    output logic [31:0] o_rdata,    // Read data (always valid, 1-cycle latency)
    output logic        o_busy      // Access in progress (for STALL)
);

    localparam ADDR_BITS = $clog2(MEM_WORDS);

    logic [31:0] mem [0:MEM_WORDS-1];

    initial begin
        for (int i = 0; i < MEM_WORDS; i++)
            mem[i] = 32'b0;
    end

    // Word-addressed, wrapping modulo MEM_WORDS
    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    // ── Latched write parameters ──────────────────────────
    // Captured at access start so the commit uses the original
    // values even if the bus changes during multi-cycle access.
    // This models real SDRAM controller behavior (command latched
    // at issue time).
    logic [ADDR_BITS-1:0] latched_addr;
    logic [31:0]          latched_wdata;
    logic [3:0]           latched_byte_en;

    // ── Synchronous write (per-byte enables) ────────────────
    // Write commits on the cycle when busy deasserts (last cycle
    // of the access), not on the first posedge. This models real
    // SDRAM write latency and ensures the STALL path is exercised.
    logic write_commit;

    always_ff @(posedge i_clk) begin
        if (write_commit) begin
            if (latched_byte_en[0]) mem[latched_addr][ 7: 0] <= latched_wdata[ 7: 0];
            if (latched_byte_en[1]) mem[latched_addr][15: 8] <= latched_wdata[15: 8];
            if (latched_byte_en[2]) mem[latched_addr][23:16] <= latched_wdata[23:16];
            if (latched_byte_en[3]) mem[latched_addr][31:24] <= latched_wdata[31:24];
        end
    end

    // ── Synchronous read ──────────────────────────────────
    // The internal register always samples the addressed word.
    // Output is poisoned while busy to catch logic that reads
    // before the access completes (fetch path, sysreg mux, etc.).
    logic [31:0] rdata_reg;
    always_ff @(posedge i_clk) begin
        rdata_reg <= mem[word_addr];
    end
    assign o_rdata = o_busy ? 32'hDEAD_BEEF : rdata_reg;

    // ── Busy counter ──────────────────────────────────────
    // Counts up from 1 to target latency. Access completes when
    // busy_count reaches target. busy is combinational — asserted
    // on the very first cycle of a request (matching the original
    // 1-cycle protocol the sequencer expects).
    //
    // Timing for READ_LATENCY=1 (original behavior):
    //   Cycle 0: i_re, in_access=0 → busy=1 (combinational)
    //            posedge: in_access=1, count=1, target=1
    //   Cycle 1: count(1)>=target(1) → busy=0, rdata valid
    //            posedge: in_access=0, count=0
    //
    // Timing for READ_LATENCY=4:
    //   Cycle 0: i_re, in_access=0 → busy=1 (combinational)
    //            posedge: in_access=1, count=1, target=4
    //   Cycle 1–3: count < target → busy=1, rdata poisoned
    //   Cycle 4: count(4)>=target(4) → busy=0, rdata valid
    //            posedge: in_access=0, count=0
    localparam MAX_LATENCY = (READ_LATENCY > WRITE_LATENCY)
                           ? READ_LATENCY : WRITE_LATENCY;
    localparam CTR_BITS = (MAX_LATENCY <= 1) ? 1 : $clog2(MAX_LATENCY + 1);

    logic [CTR_BITS-1:0] busy_count;
    logic [CTR_BITS-1:0] target;
    logic in_access;       // high while counting
    logic is_write;        // tracks whether current access is a write

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            busy_count <= '0;
            target     <= '0;
            in_access  <= 1'b0;
            is_write   <= 1'b0;
        end else if (!in_access && (i_re || i_we)) begin
            // New access — start counting, latch parameters
            in_access      <= 1'b1;
            busy_count     <= CTR_BITS'(1);
            target         <= i_we ? WRITE_LATENCY[CTR_BITS-1:0]
                                   : READ_LATENCY[CTR_BITS-1:0];
            is_write       <= i_we;
            latched_addr   <= word_addr;
            latched_wdata  <= i_wdata;
            latched_byte_en <= i_byte_en;
        end else if (in_access && busy_count >= target) begin
            // Access complete — return to idle
            in_access  <= 1'b0;
            busy_count <= '0;
        end else if (in_access) begin
            busy_count <= busy_count + CTR_BITS'(1);
        end
    end

    // Combinational busy: asserted on the first request cycle
    // (in_access still 0) AND while counting hasn't reached target.
    logic access_complete;
    assign access_complete = in_access && (busy_count >= target);
    assign o_busy          = (i_re || i_we) && !access_complete;
    assign write_commit    = access_complete && is_write;

endmodule

// verilator lint_on UNUSEDSIGNAL
