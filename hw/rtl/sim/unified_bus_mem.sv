// unified_bus_mem — bus-shaped unified ROM/RAM memory device.
//
// The single-port, bus-attached sibling of unified_mem: the same
// region-compressed address map (addr[31] selects ROM high / RAM low, the
// low bits index within; INIT_FILE loads into the ROM region) presented as
// a Penumbra Bus device for a machine's external bus — the memory behind
// machine_penumbra2 in the program-runner wrapper and the synthesis probe.
//
// Bus contract (the registered-read device rule, hw/CLAUDE.md): a read
// asserts o_busy for one cycle while the registered read settles, then
// presents the data on the busy-drop cycle and holds it (the
// access_pending pattern). Writes are byte-enabled, complete in the cycle
// they are presented (o_busy stays low), and are dropped — and flagged in
// sim — when aimed at the ROM region.

// High address bits above the region index, and the low two byte-select
// bits, are intentionally unused (word-indexed within a region).
// verilator lint_off UNUSEDSIGNAL

module unified_bus_mem #(
    parameter int    REGION_WORDS = 4096,    // words per region (RAM, ROM)
    parameter string INIT_FILE    = ""       // image loaded into the ROM region
)(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_re,
    input  logic        i_we,
    output logic [31:0] o_rdata,
    output logic        o_busy
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

    logic [IDX_W:0] idx;
    assign idx = {i_addr[31], i_addr[IDX_W+1:2]};

    // ── Read: registered, 1-cycle busy (access_pending pattern) ──
    logic access_pending;
    assign o_busy = i_re && !access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            access_pending <= 1'b0;
        end else begin
            access_pending <= o_busy;      // the launched read completes next cycle
            if (o_busy)
                o_rdata <= mem[idx];
        end
    end

    // ── Write: byte-enabled, single-cycle, RAM region only ───────
    always_ff @(posedge i_clk) begin
        if (i_we && !i_addr[31]) begin
            if (i_byte_en[0]) mem[idx][ 7: 0] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem[idx][15: 8] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem[idx][23:16] <= i_wdata[23:16];
            if (i_byte_en[3]) mem[idx][31:24] <= i_wdata[31:24];
        end
    end

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A write to the ROM region is a software/wiring bug — real ROM would
    // drop it silently, so flag it here rather than let it vanish.
    always_ff @(posedge i_clk)
        assert (!(i_we && i_addr[31]))
            else $error("unified_bus_mem: write to the read-only ROM region (addr=0x%08x)", i_addr);

endmodule

// verilator lint_on UNUSEDSIGNAL
