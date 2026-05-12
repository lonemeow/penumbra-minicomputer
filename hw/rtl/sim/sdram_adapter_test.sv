// Unit-test wrapper for sdram_bus_adapter.
//
// Wires the adapter to a behavioral two-slot mock memory in place of
// the real SDRAM controller + CDC + chip model.  The mock issues
// rsp_valid (reads) or o_done-only (writes) with a parameterized
// per-request latency, in arrival order.  Read data is deterministic:
//
//     rsp_data = MOCK_TAG | (addr & 32'hFFFF_FFFC)
//
// so a misrouted spec response is detectable as a wrong value, not a
// 50% chance of looking right.  The mock holds at most two requests
// in flight (matching the depth-2 CDC the real controller pair
// presents), and supports a single-bit stall input that gates
// req_ready for back-pressure testing.
//
// The latency input is a per-request count of clock cycles between
// acceptance and the response/done pulse:
//   latency = 1  → response arrives in the cycle after accept.
//   latency = N  → response arrives N cycles after accept.
//
// Adapter internals (tag_count, tag_fifo, spec_in_flight,
// spec_buffered, spec_addr) are exposed via debug ports for
// testbench observability and finer-grained assertions.

module sdram_adapter_test (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Cache-facing bus (DUT inputs) ─────────────────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Mock-memory controls ──────────────────────────────────
    // Per-request response latency in cycles (clamped to ≥1).
    input  logic [7:0]  i_mock_latency,
    // When high, the mock holds req_ready low (back-pressure).
    input  logic        i_mock_stall,

    // ── Debug taps into adapter internals ─────────────────────
    output logic [1:0]  o_dbg_tag_count,
    output logic [1:0]  o_dbg_tag_fifo,
    output logic        o_dbg_spec_in_flight,
    output logic        o_dbg_spec_buffered,
    output logic [31:0] o_dbg_spec_addr,

    // ── Mock observability ────────────────────────────────────
    output logic        o_mock_slot0_valid,
    output logic        o_mock_slot1_valid
);

    localparam logic [31:0] MOCK_TAG = 32'hAA00_0000;

    // ── Adapter ↔ mock signals ────────────────────────────────
    logic        req_valid;
    logic        req_we;
    logic [31:0] req_addr;
    logic [31:0] req_wdata;
    logic [3:0]  req_byte_en;
    logic        req_ready;

    logic        rsp_valid;
    logic [31:0] rsp_data;
    logic        rsp_done;

    // The mock memory does not model store data — writes are tracked
    // only as tag-FIFO traffic with an o_done pulse, which is what
    // the adapter's bookkeeping consumes.  Silence the lint warning
    // on the unread wdata / byte_en lanes.
    /* verilator lint_off UNUSEDSIGNAL */
    wire _mock_unused = &{1'b0, req_wdata, req_byte_en};
    /* verilator lint_on UNUSEDSIGNAL */

    /* verilator lint_off PINCONNECTEMPTY */
    sdram_bus_adapter u_dut (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_addr        (i_addr),
        .i_wdata       (i_wdata),
        .i_byte_en     (i_byte_en),
        .i_we          (i_we),
        .i_re          (i_re),
        .o_rdata       (o_rdata),
        .o_busy        (o_busy),
        .o_req_valid   (req_valid),
        .o_req_we      (req_we),
        .o_req_addr    (req_addr),
        .o_req_wdata   (req_wdata),
        .o_req_byte_en (req_byte_en),
        .i_req_ready   (req_ready),
        .i_rsp_valid   (rsp_valid),
        .i_rsp_data    (rsp_data),
        .o_rsp_ready   (),
        .i_done        (rsp_done)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // Expose adapter internals for the testbench.
    assign o_dbg_tag_count      = u_dut.tag_count;
    assign o_dbg_tag_fifo       = u_dut.tag_fifo;
    assign o_dbg_spec_in_flight = u_dut.spec_in_flight;
    assign o_dbg_spec_buffered  = u_dut.spec_buffered;
    assign o_dbg_spec_addr      = u_dut.spec_addr;

    // ── Behavioral mock memory ────────────────────────────────
    // Two-slot in-order pipeline.  slot[0] is the head (closest to
    // completion); slot[1] is the younger entry.  Each entry holds
    // its absolute completion cycle; the head completes when the
    // free-running mock_cycle counter reaches that value.

    typedef struct packed {
        logic        valid;
        logic        we;
        logic [31:0] addr;
        logic [31:0] ready_at;
    } slot_t;

    slot_t slot_q [2];
    slot_t slot_d [2];
    logic [31:0] mock_cycle;

    assign o_mock_slot0_valid = slot_q[0].valid;
    assign o_mock_slot1_valid = slot_q[1].valid;

    wire complete_now = slot_q[0].valid && (mock_cycle == slot_q[0].ready_at);

    // Accept when slot[1] is free, or when slot[0] is completing this
    // cycle (slot[1] will vacate to slot[0] at the next clock edge).
    assign req_ready = !i_mock_stall && (!slot_q[1].valid || complete_now);

    // Response / done pulses driven the cycle the head completes.
    assign rsp_valid = complete_now && !slot_q[0].we;
    assign rsp_data  = MOCK_TAG | (slot_q[0].addr & 32'hFFFF_FFFC);
    assign rsp_done  = complete_now;

    wire [31:0] lat_eff   = (i_mock_latency == 8'd0)
                            ? 32'd1
                            : {24'd0, i_mock_latency};
    wire [31:0] new_ready = mock_cycle + lat_eff;

    always_comb begin
        // Default: hold.
        slot_d[0] = slot_q[0];
        slot_d[1] = slot_q[1];

        // Completion: shift slot[1] into slot[0], clear slot[1].
        if (complete_now) begin
            slot_d[0] = slot_q[1];
            slot_d[1] = '0;
        end

        // Push: land in the first free slot post-shift.
        if (req_valid && req_ready) begin
            if (!slot_d[0].valid)
                slot_d[0] = '{valid:1'b1, we:req_we, addr:req_addr, ready_at:new_ready};
            else
                slot_d[1] = '{valid:1'b1, we:req_we, addr:req_addr, ready_at:new_ready};
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            slot_q[0]  <= '0;
            slot_q[1]  <= '0;
            mock_cycle <= 32'd0;
        end else begin
            slot_q[0]  <= slot_d[0];
            slot_q[1]  <= slot_d[1];
            mock_cycle <= mock_cycle + 32'd1;
        end
    end

endmodule
