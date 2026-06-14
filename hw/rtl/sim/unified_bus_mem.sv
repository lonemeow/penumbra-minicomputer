// unified_bus_mem — bus-shaped unified ROM/RAM memory device.
//
// The single-port, bus-attached sibling of unified_mem: a RAM region at
// RAM_BASE and a ROM region at ROM_BASE (INIT_FILE / +rom_hex loads the ROM
// region), presented as a Penumbra Bus slave for a machine's external bus —
// the memory behind machine_penumbra2 in the program-runner wrapper and the
// synthesis probe.
//
// Address ownership is the device's, not the fabric's. On the external async
// bus, what is mapped where is discovered at boot (autoconfig), so no master-
// or fabric-side decoder can hold a static map — a no-device access is a bus-
// protocol fault (no slave acknowledges; a watchdog/timeout in the async form).
// So this slave decodes only the addresses it actually backs, from its own
// size (REGION_WORDS — the same parameter that sizes the storage): it responds
// and asserts o_claimed for an in-region access, and is silent otherwise. The
// fabric raises bus_fault when no slave claims an access; this device just
// declines to claim what it does not back. (The storage is region-compressed —
// two REGION_WORDS arrays — so the backed extent equals REGION_WORDS per
// region, with nothing aliased in: an out-of-region address is unclaimed, not
// wrapped.)
//
// Bus contract (the registered-read device rule, hw/CLAUDE.md): a claimed read
// asserts o_busy for one cycle while the registered read settles, then presents
// the data on the busy-drop cycle and holds it (the access_pending pattern).
// Writes are byte-enabled, complete in the cycle they are presented (o_busy
// stays low), and are dropped — and flagged in sim — when aimed at the ROM
// region.

// verilator lint_off UNUSEDSIGNAL

module unified_bus_mem
    import penumbra_pkg::*;
#(
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
    output logic        o_busy,
    output logic        o_claimed   // this slave backs i_addr (combinational decode)
);

    localparam int          IDX_W        = $clog2(REGION_WORDS); // word index within a region
    localparam int          N            = 2 * REGION_WORDS;     // RAM region | ROM region
    localparam logic [31:0] REGION_BYTES = 32'(REGION_WORDS) * 4;

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

    // ── Address ownership: decode this device's own backed regions ──
    // Subtract-then-compare so the ROM region (at the top of the address
    // space) decodes without overflow. An address outside both regions is
    // not ours — o_claimed stays low and the fabric faults it.
    logic ram_hit, rom_hit;
    assign ram_hit   = (i_addr - RAM_BASE) < REGION_BYTES;
    assign rom_hit   = (i_addr - ROM_BASE) < REGION_BYTES;
    assign o_claimed = ram_hit | rom_hit;

    // Region select + word index. RAM_BASE/ROM_BASE differ in bit 31, so it
    // selects the region; the low bits index within (offset/4). Valid only for
    // a claimed access — an unclaimed one neither reads nor writes.
    logic [IDX_W:0] idx;
    assign idx = {i_addr[31], i_addr[IDX_W+1:2]};

    // ── Read: registered, 1-cycle busy (access_pending pattern) ──
    // Only a claimed read responds; an unclaimed read leaves o_busy low and
    // the fabric's no-slave fault completes it.
    logic access_pending;
    assign o_busy = i_re && o_claimed && !access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            access_pending <= 1'b0;
        end else begin
            access_pending <= o_busy;      // the launched read completes next cycle
            if (o_busy)
                o_rdata <= mem[idx];
        end
    end

    // ── Write: byte-enabled, single-cycle, claimed RAM region only ───────
    always_ff @(posedge i_clk) begin
        if (i_we && ram_hit) begin
            if (i_byte_en[0]) mem[idx][ 7: 0] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem[idx][15: 8] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem[idx][23:16] <= i_wdata[23:16];
            if (i_byte_en[3]) mem[idx][31:24] <= i_wdata[31:24];
        end
    end

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A write to the (claimed) ROM region is a software/wiring bug — real ROM
    // would drop it silently, so flag it here rather than let it vanish.
    always_ff @(posedge i_clk)
        assert (!(i_we && rom_hit))
            else $error("unified_bus_mem: write to the read-only ROM region (addr=0x%08x)", i_addr);

endmodule

// verilator lint_on UNUSEDSIGNAL
