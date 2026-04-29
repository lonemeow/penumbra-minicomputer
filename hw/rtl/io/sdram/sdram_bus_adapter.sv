// SDRAM bus adapter — synchronous bus ↔ controller req/rsp.
//
// Translates the project's standard synchronous device-bus interface
// (i_re/i_we/o_busy/o_rdata, used by simple_mem, fpga_ram, boot_rom)
// into the controller's req_valid/req_ready/rsp_valid handshake.
// Drop-in replacement for `simple_mem` at the SDRAM region.
//
// Single-word transactions today.  When the CDC bridge is added in
// step 4 of the design plan, the bus adapter stays on the system-clock
// side and the CDC bridge sits between adapter and controller.
//
// o_busy contract (per `hw/CLAUDE.md` § Memory Access):
//   • o_busy is high while a request is being served.
//   • o_busy drops on the *same* cycle that o_rdata is valid (reads)
//     or that the write data has been committed (writes).
//   • The cache stalls while o_busy is high; on the cycle it goes
//     low, the cache latches o_rdata and de-asserts i_re/i_we.
//
// State machine:
//   ADP_IDLE       → no transaction in flight; o_busy = (i_re|i_we)
//   ADP_WAIT_REQ   → driving req_valid, waiting for controller's
//                    one-cycle req_ready acceptance pulse
//   ADP_WAIT_DONE  → request accepted, waiting for controller's
//                    o_done pulse (and rsp_valid for reads)
//   ADP_PRESENT    → drop o_busy, present rdata for one cycle so
//                    the cache can sample it
//
// The PRESENT state adds one cycle of latency between controller's
// o_done and the cache observing !o_busy.  This is acceptable
// overhead and keeps the data path purely registered.

module sdram_bus_adapter (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Cache-facing bus ─────────────────────────────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Controller-facing handshake ──────────────────────────
    output logic        o_req_valid,
    output logic        o_req_we,
    output logic [31:0] o_req_addr,
    output logic [31:0] o_req_wdata,
    output logic [3:0]  o_req_byte_en,
    input  logic        i_req_ready,

    input  logic        i_rsp_valid,
    input  logic [31:0] i_rsp_data,
    output logic        o_rsp_ready,

    input  logic        i_done
);

    typedef enum logic [1:0] {
        ADP_IDLE,
        ADP_WAIT_REQ,
        ADP_WAIT_DONE,
        ADP_PRESENT
    } state_t;

    state_t state;

    // Latched payload — held stable while req_valid is asserted.
    logic        req_we_r;
    logic [31:0] req_addr_r;
    logic [31:0] req_wdata_r;
    logic [3:0]  req_byte_en_r;
    logic        was_read;
    logic [31:0] rdata_latched;

    assign o_req_valid   = (state == ADP_WAIT_REQ);
    assign o_req_we      = req_we_r;
    assign o_req_addr    = req_addr_r;
    assign o_req_wdata   = req_wdata_r;
    assign o_req_byte_en = req_byte_en_r;
    assign o_rsp_ready   = 1'b1;            // always ready to consume
    assign o_rdata       = rdata_latched;

    // o_busy contract: high whenever a transaction is in progress, OR
    // when the cache is asking but we haven't latched it yet.
    // Low only in PRESENT (data being served) or in IDLE without an
    // incoming request.
    assign o_busy =
        (state == ADP_WAIT_REQ) || (state == ADP_WAIT_DONE) ||
        ((state == ADP_IDLE) && (i_re || i_we));

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state          <= ADP_IDLE;
            req_we_r       <= 1'b0;
            req_addr_r     <= '0;
            req_wdata_r    <= '0;
            req_byte_en_r  <= '0;
            was_read       <= 1'b0;
            rdata_latched  <= '0;
        end else begin
            case (state)
                ADP_IDLE: begin
                    if (i_re || i_we) begin
                        req_we_r       <= i_we;
                        req_addr_r     <= i_addr;
                        req_wdata_r    <= i_wdata;
                        req_byte_en_r  <= i_byte_en;
                        was_read       <= i_re;
                        state          <= ADP_WAIT_REQ;
                    end
                end

                ADP_WAIT_REQ: begin
                    if (i_req_ready) state <= ADP_WAIT_DONE;
                end

                ADP_WAIT_DONE: begin
                    // For reads, rsp_valid and done coincide.  For
                    // writes there's no rsp_valid — only done.
                    if (was_read && i_rsp_valid) begin
                        rdata_latched <= i_rsp_data;
                        state         <= ADP_PRESENT;
                    end else if (!was_read && i_done) begin
                        state <= ADP_PRESENT;
                    end
                end

                ADP_PRESENT: begin
                    state <= ADP_IDLE;
                end

                default: state <= ADP_IDLE;
            endcase
        end
    end

endmodule
