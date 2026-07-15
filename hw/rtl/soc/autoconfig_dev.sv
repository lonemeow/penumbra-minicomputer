// Autoconfig Device Wrapper — adds bus discovery to any device
//
// Wraps an inner device with the autoconfig protocol: config space
// registers, cfg daisy chain, dynamic address decode from latched
// base address. The inner device sees a standard bus interface and
// doesn't know it's autoconfigured.
//
// Protocol: software must deassert and reassert CFG_EN between
// configuring each device. This lets the chain settle before the
// next device becomes active, avoiding write-through races.
//
// Config space (active when cfg_in=1 and device is unconfigured):
//   0x00  CFG_CLASS  (R)  — device class / base protocol
//   0x04  CFG_SIZE   (R)  — required address space (bytes, power-of-2)
//   0x08  CFG_ID     (R)  — manufacturer/product ID (0 = generic)
//   0x0C  CFG_NAME0  (R)  — device name bytes  0-3 (packed LE)
//   0x10  CFG_NAME1  (R)  — device name bytes  4-7
//   0x14  CFG_NAME2  (R)  — device name bytes  8-11
//   0x18  CFG_NAME3  (R)  — device name bytes 12-15
//   0x1C  CFG_BASE   (W)  — write assigned base → device enables
//
// Discrete 74xx: one flip-flop (configured), one latch (base addr),
// one comparator (dynamic address decode), config ROM (pull-ups).

// verilator lint_off UNUSEDSIGNAL

module autoconfig_dev
    import penumbra_pkg::*;
#(
    // ── Config space identity ──────────────────────────────
    parameter logic [31:0] DEV_CLASS = ACFG_CLASS_UNKNOWN,
    parameter logic [31:0] DEV_SIZE  = 32'd4096,     // must be power-of-2
    parameter logic [31:0] DEV_ID    = 32'd0,         // 0 = generic
    parameter logic [31:0] DEV_NAME0 = 32'h00000000,  // packed LE, null-padded
    parameter logic [31:0] DEV_NAME1 = 32'h00000000,
    parameter logic [31:0] DEV_NAME2 = 32'h00000000,
    parameter logic [31:0] DEV_NAME3 = 32'h00000000
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Bus reset from bus controller ──────────────────────
    input  logic        i_bus_rst,

    // ── Config chain ───────────────────────────────────────
    input  logic        i_cfg_en,      // CFG_EN from bus controller
    input  logic        i_cfg_in,      // Daisy chain input
    output logic        o_cfg_out,     // Daisy chain output

    // ── Memory bus (directly from shared bus) ──────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,

    // ── Bus response (directly to OR-combine) ──────────────
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_sel,         // Device selected this cycle (for bus fault)

    // ── Inner device bus interface ─────────────────────────
    output logic [31:0] o_dev_addr,
    output logic [31:0] o_dev_wdata,
    output logic [3:0]  o_dev_byte_en,
    output logic        o_dev_we,
    output logic        o_dev_re,
    input  logic [31:0] i_dev_rdata,
    input  logic        i_dev_busy
);

    // ── Config space address decode ────────────────────────
    logic cfg_space_sel;
    assign cfg_space_sel = (i_addr[31:5] == AUTOCONFIG_BASE[31:5]);

    // Active when: CFG_EN set, this device is first unconfigured
    // in chain (cfg_in=1), and address is in config range.
    logic cfg_active;
    assign cfg_active = i_cfg_en & i_cfg_in & !configured & cfg_space_sel;

    // ── Config state ───────────────────────────────────────
    logic        configured;
    logic [31:0] base_addr;

    always_ff @(posedge i_clk) begin
        if (i_rst || i_bus_rst) begin
            configured <= 1'b0;
            base_addr  <= 32'b0;
        end else if (cfg_active && i_we && i_addr[4:2] == 3'b111) begin
            // Write to CFG_BASE (offset 0x1C) → latch and enable
            base_addr  <= i_wdata;
            configured <= 1'b1;
        end
    end

    // ── Config chain output ────────────────────────────────
    // Only pass cfg through when configured AND cfg_en was
    // deasserted since configuration. This prevents the chain
    // from propagating during the same bus cycle that configured
    // this device. Software must toggle CFG_EN between devices.
    logic cfg_seen_low;
    always_ff @(posedge i_clk) begin
        if (i_rst || i_bus_rst)
            cfg_seen_low <= 1'b0;
        else if (!i_cfg_en)
            cfg_seen_low <= 1'b1;
        else if (!configured)
            cfg_seen_low <= 1'b0;
    end
    assign o_cfg_out = i_cfg_in & configured & cfg_seen_low;

    // ── Config space read mux ──────────────────────────────
    logic [31:0] cfg_rdata;
    always_comb begin
        case (i_addr[4:2])
            3'd0: cfg_rdata = DEV_CLASS;   // 0x00
            3'd1: cfg_rdata = DEV_SIZE;    // 0x04
            3'd2: cfg_rdata = DEV_ID;      // 0x08
            3'd3: cfg_rdata = DEV_NAME0;   // 0x0C
            3'd4: cfg_rdata = DEV_NAME1;   // 0x10
            3'd5: cfg_rdata = DEV_NAME2;   // 0x14
            3'd6: cfg_rdata = DEV_NAME3;   // 0x18
            3'd7: cfg_rdata = 32'b0;       // 0x1C (write-only)
            default: cfg_rdata = 32'b0;
        endcase
    end

    // ── Dynamic address decode (configured state) ──────────
    logic dev_sel;
    assign dev_sel = configured
                   & ((i_addr & ~(DEV_SIZE - 1)) == base_addr);

    // ── Read data output ───────────────────────────────────
    // Track whether previous cycle was a config read or device read.
    // Parent module gates our o_rdata with _sel_r to prevent
    // stale data leaking through the bus OR-combine.
    logic was_cfg;
    always_ff @(posedge i_clk) begin
        if (i_rst || i_bus_rst)
            was_cfg <= 1'b0;
        else
            was_cfg <= cfg_active;
    end

    // Config data: registered for 1-cycle read latency.
    logic [31:0] cfg_rdata_r;
    always_ff @(posedge i_clk)
        cfg_rdata_r <= cfg_rdata;

    // Mux between config data (registered here) and inner device
    // data (registered inside the inner device).
    assign o_rdata = was_cfg ? cfg_rdata_r : i_dev_rdata;

    // ── Busy ───────────────────────────────────────────────
    // Config reads assert busy for 1 cycle so the registered
    // select in the bus OR-combine has time to propagate
    // before the CPU samples mem_rdata. Config writes must NOT
    // stall: the CFG_BASE write latches `configured` on its
    // first edge, which deselects the config space — a stalled
    // write's held strobe would then sit on the bus unclaimed
    // and raise a spurious bus fault on its completion cycle.
    logic cfg_was_active;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            cfg_was_active <= 1'b0;
        else
            cfg_was_active <= cfg_active && i_re;
    end
    wire cfg_busy = cfg_active && i_re && !cfg_was_active;
    assign o_busy = cfg_busy | (dev_sel ? i_dev_busy : 1'b0);

    // ── Selection (for bus fault detection) ─────────────────
    assign o_sel = cfg_active | dev_sel;

    // ── Inner device bus forwarding ────────────────────────
    assign o_dev_addr    = i_addr;
    assign o_dev_wdata   = i_wdata;
    assign o_dev_byte_en = i_byte_en;
    assign o_dev_we      = i_we & dev_sel;
    assign o_dev_re      = i_re & dev_sel;

endmodule
