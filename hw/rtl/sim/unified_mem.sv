// unified_mem — dual-port unified memory stand-in for the gen2 core.
//
// Models the physical memory the real system's split L1 I/D caches back to:
// the caches are separate BRAMs, but they share one physical address space, so
// a store is visible to a later fetch and there is a single "physical 0x0".
// This stand-in is that backing store, presented through two ports so a fetch
// and a data access can proceed the same cycle — the role the two L1 caches
// play in the real machine. Real BRAM-backed caches replace it later behind
// the IF and dmem interfaces.
//
// Both ports follow the registered-read BRAM contract (the contract the gen2
// fetch/data sides are built around): the address is sampled at the clock edge
// and the addressed word appears combinationally the next cycle; a clock-enable
// (i_*_en) freezes the output to hold it in lockstep with a stalled stage.
//
//   - Port A — instruction fetch: read-only.
//   - Port B — data: byte-enabled write + read, no read/write-through (a read
//     the cycle a word is written returns the old value).
//
// Address map. The backed physical regions are far apart — RAM low, ROM at
// 0xFFFF_0000 — so a flat array large enough to hold both is impossible. The
// stand-in compresses them: addr[31] selects the region (0 = RAM, 1 = ROM),
// the low bits index within it, and the two regions occupy disjoint halves of
// one array. This is a miniature of the real bus address map, and it keeps the
// reset code (0xFFFF_0000) and the vector table (0x0) from aliasing the way a
// single wrapped array would. ROM is read-only: a write to a region-1 address
// is dropped (and flagged in sim), matching real ROM. INIT_FILE loads into the
// ROM region.

// High address bits above the region index, and the low two byte-select bits,
// are intentionally unused (the address is word-indexed within a region).
// verilator lint_off UNUSEDSIGNAL

module unified_mem #(
    parameter int    REGION_WORDS = 4096,    // words per region (RAM, ROM)
    parameter string INIT_FILE    = ""       // image loaded into the ROM region
)(
    input  logic        i_clk,

    // ── Port A — instruction fetch (read-only) ───────────────────
    input  logic [31:0] i_a_addr,
    input  logic        i_a_en,              // read clock-enable
    output logic [31:0] o_a_rdata,

    // ── Port B — data (read/write, byte-enabled) ─────────────────
    input  logic [31:0] i_b_addr,
    input  logic [31:0] i_b_wdata,
    input  logic [3:0]  i_b_byte_en,
    input  logic        i_b_we,
    input  logic        i_b_en,              // read clock-enable
    output logic [31:0] o_b_rdata
);

    localparam int IDX_W = $clog2(REGION_WORDS);   // word index within a region
    localparam int N     = 2 * REGION_WORDS;       // RAM region | ROM region

    logic [31:0] mem [0:N-1];

`ifdef VERILATOR
    // Simulation: +rom_hex=<path> overrides INIT_FILE, so per-program
    // testbench images live under build/ instead of contending for one
    // shared filename.
    string init_file;
`endif

    // Zero-init is common to simulation and synthesis; *loading* an image
    // is a simulation mechanism (the synthesizable consumer — the gen2
    // timing probe — runs the core against a zeroed memory).
    initial begin
        for (int i = 0; i < N; i++)
            mem[i] = 32'b0;
`ifdef VERILATOR
        if (!$value$plusargs("rom_hex=%s", init_file))
            init_file = INIT_FILE;
        if (init_file != "")
            $readmemh(init_file, mem, REGION_WORDS);   // ROM region starts at REGION_WORDS
`endif
    end

    // Region-compressed index: addr[31] picks the half, the low bits index
    // within. The two halves never alias because they differ in the top bit.
    logic [IDX_W:0] a_idx, b_idx;
    assign a_idx = {i_a_addr[31], i_a_addr[IDX_W+1:2]};
    assign b_idx = {i_b_addr[31], i_b_addr[IDX_W+1:2]};

    // ── Port A: registered read (fetch) ──────────────────────────
    always_ff @(posedge i_clk)
        if (i_a_en) o_a_rdata <= mem[a_idx];

    // ── Port B: byte-enabled write (RAM only) + registered read ──
    // ROM (region 1) ignores writes — the read picks up the old value. The
    // read samples before the write commits (non-blocking), so a same-address
    // read/write returns the old word.
    always_ff @(posedge i_clk) begin
        if (i_b_we && !i_b_addr[31]) begin
            if (i_b_byte_en[0]) mem[b_idx][ 7: 0] <= i_b_wdata[ 7: 0];
            if (i_b_byte_en[1]) mem[b_idx][15: 8] <= i_b_wdata[15: 8];
            if (i_b_byte_en[2]) mem[b_idx][23:16] <= i_b_wdata[23:16];
            if (i_b_byte_en[3]) mem[b_idx][31:24] <= i_b_wdata[31:24];
        end
        if (i_b_en) o_b_rdata <= mem[b_idx];
    end

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A data store to the ROM region is a software/wiring bug — real ROM
    // would drop it silently, so flag it here rather than let it vanish.
    always_ff @(posedge i_clk)
        assert (!(i_b_we && i_b_addr[31]))
            else $error("unified_mem: data write to the read-only ROM region (addr=0x%08x)", i_b_addr);

endmodule

// verilator lint_on UNUSEDSIGNAL
