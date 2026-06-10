// Penumbra Boot ROM — read-only memory for simulation
//
// Parameterizable size, word-addressed. Loads program.hex at init.
// Synchronous read with 1-cycle latency (same protocol as simple_mem).
// Read-only: writes are silently ignored.
//
// In the real SoC this would be replaced by actual ROM/flash
// containing the bootloader.

// verilator lint_off UNUSEDSIGNAL

module boot_rom
    import penumbra_pkg::*;
#(
    parameter ROM_WORDS = 16384  // Default 16K words = 64 KB
)
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,     // Byte address (lower bits used)
    input  logic        i_re,       // Read enable
    output logic [31:0] o_rdata,    // Read data (1-cycle latency)
    output logic        o_busy      // Access in progress (for STALL)
);

    localparam ADDR_BITS = $clog2(ROM_WORDS);

    logic [31:0] rom [0:ROM_WORDS-1];

`ifdef VERILATOR
    // Simulation: +rom_hex=<path> selects the image, so testbench programs
    // live in per-program files instead of contending for the boot ROM's
    // program.hex (the synthesis default below).
    string init_file;
`endif

    initial begin
        for (int i = 0; i < ROM_WORDS; i++)
            rom[i] = 32'b0;
`ifdef VERILATOR
        if (!$value$plusargs("rom_hex=%s", init_file))
            init_file = "program.hex";
        $readmemh(init_file, rom);
`else
        $readmemh("program.hex", rom);
`endif
    end

    // Word-addressed (lower bits index into ROM)
    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    // ── Synchronous read (1-cycle latency) ─────────────────
    logic [31:0] rdata_reg;
    always_ff @(posedge i_clk) begin
        rdata_reg <= rom[word_addr];
    end
    assign o_rdata = rdata_reg;

    // ── Busy signal (same protocol as simple_mem) ──────────
    logic access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_pending <= 1'b0;
        else if (i_re && !access_pending)
            access_pending <= 1'b1;
        else
            access_pending <= 1'b0;
    end

    assign o_busy = i_re && !access_pending;

endmodule

// verilator lint_on UNUSEDSIGNAL
