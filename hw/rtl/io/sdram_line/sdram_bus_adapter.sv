// SDRAM bus adapter — line-buffer version.
//
// Bridges the sync bus (i_re/i_we/o_ack/o_rdata) to the controller's
// req/rsp handshake, holding the last line the controller returned.
//
// The bus can only ever ask for one word, so a device that wants to
// exploit sequential access has to infer it.  The speculative version
// infers it by guessing the next address and racing a prefetch against
// the cache; this one lets the controller answer a whole line at once
// and keeps what it returns.  A cache fill walks four consecutive
// words, so one miss pays the command, the row activation and the CAS
// wait, and the three words after it are answered without touching the
// controller at all.
//
// That also makes the adapter simpler rather than more complex: one
// request is outstanding at a time, so there is no speculation to
// abandon, no response to route between a real and a speculative
// consumer, and no tag FIFO to keep them apart.
//
// o_ack contract (doc/hardware/bus-protocol.md, "Sync handshake"):
//   • o_ack is high for the one cycle an access completes: o_rdata
//     valid (reads), or the write accepted by the CDC (writes).
//   • It decodes the FSM state alone (ADP_PRESENT), so no path runs
//     from a request to its own completion within a cycle.
//   • The master holds its request through that cycle; a strobe still
//     held in the cycle after is its next access (PRESENT→BEGIN).
//
// State machine:
//   ADP_IDLE     → no transaction in flight
//   ADP_BEGIN    → request live; a read either hits the held line or
//                  pushes a line request, a write pushes its word
//   ADP_WAIT_RSP → line request accepted, awaiting the response
//   ADP_PRESENT  → raise o_ack, present rdata for one cycle
//
// Writes never enter ADP_WAIT_RSP.  Once the CDC accepts a write into
// its slot, ordering is locked (CDC FIFO + serial controller), so any
// later access to the same address lands after it; the cache is
// released the moment req_ready pulses.

module sdram_bus_adapter #(
    // Width of one controller response.  The controller answers with a
    // whole burst, so this is the line the adapter holds.
    parameter int RSP_BITS = 128
) (
    input  logic                i_clk,
    input  logic                i_rst,

    // ── Cache-facing bus ─────────────────────────────────────
    input  logic [31:0]         i_addr,
    input  logic [31:0]         i_wdata,
    input  logic [3:0]          i_byte_en,
    input  logic                i_we,
    input  logic                i_re,
    output logic [31:0]         o_rdata,
    output logic                o_ack,

    // ── Controller-facing handshake ──────────────────────────
    output logic                o_req_valid,
    output logic                o_req_we,
    output logic [31:0]         o_req_addr,
    output logic [31:0]         o_req_wdata,
    output logic [3:0]          o_req_byte_en,
    input  logic                i_req_ready,

    input  logic                i_rsp_valid,
    input  logic [RSP_BITS-1:0] i_rsp_data,
    output logic                o_rsp_ready,

    input  logic                i_done
);

    // i_done reports a transaction retiring, which this adapter does not
    // need: one request is outstanding at a time, so a read completes on
    // its response and a write the moment the CDC takes it.
    /* verilator lint_off UNUSEDSIGNAL */
    wire _unused = &{1'b0, i_done};
    /* verilator lint_on UNUSEDSIGNAL */

    // Line geometry follows the response width: the controller decides
    // how much arrives at once, and the adapter holds exactly that.
    localparam int LINE_WORDS = RSP_BITS / 32;
    localparam int WORD_BITS  = $clog2(LINE_WORDS);
    localparam int OFF_BITS   = WORD_BITS + 2;          // byte offset in a line

    typedef enum logic [1:0] {
        ADP_IDLE,
        ADP_BEGIN,
        ADP_WAIT_RSP,
        ADP_PRESENT
    } state_t;

    state_t state;

    logic [31:0]         rdata_latched;

    // ── Held line ────────────────────────────────────────────
    logic                line_valid;
    logic [31:OFF_BITS]  line_tag;       // address bits above the line
    logic [RSP_BITS-1:0] line_data;

    // What the live request wants, latched so the response path does
    // not depend on the bus still driving the address.
    logic [WORD_BITS-1:0] req_word;
    logic [31:OFF_BITS]   req_tag;

    wire [31:OFF_BITS]    addr_tag  = i_addr[31:OFF_BITS];
    wire [WORD_BITS-1:0]  addr_word = i_addr[OFF_BITS-1:2];

    // Whether the held line covers this address, whichever way the
    // access goes: a read serves out of it, a write has to keep it
    // consistent.  Direction is the caller's business.
    wire line_hit = line_valid && (addr_tag == line_tag);

    // The word a response carries for the request waiting on it.
    wire [31:0] rsp_word = i_rsp_data[{req_word, 5'b0} +: 32];

    // ── Request drive ────────────────────────────────────────
    // A read that misses asks for the line containing it; the burst
    // aligns down anyway, so the address it sends is the line base.
    wire pushing_read  = (state == ADP_BEGIN) && i_re && !line_hit;
    wire pushing_write = (state == ADP_BEGIN) && i_we;

    assign o_req_valid   = pushing_read || pushing_write;
    assign o_req_we      = pushing_write;
    assign o_req_addr    = pushing_write ? i_addr
                                         : {i_addr[31:OFF_BITS], {OFF_BITS{1'b0}}};
    assign o_req_wdata   = i_wdata;
    assign o_req_byte_en = pushing_write ? i_byte_en : 4'hF;
    assign o_rsp_ready   = 1'b1;

    assign o_rdata       = rdata_latched;
    assign o_ack         = (state == ADP_PRESENT);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state         <= ADP_IDLE;
            rdata_latched <= '0;
            line_valid    <= 1'b0;
            line_tag      <= '0;
            line_data     <= '0;
            req_word      <= '0;
            req_tag       <= '0;
        end else begin
            // ── Response capture ─────────────────────────────
            // One request is outstanding at a time, so a response can
            // only belong to the read that is waiting for it.
            if (i_rsp_valid) begin
                line_data  <= i_rsp_data;
                line_tag   <= req_tag;
                line_valid <= 1'b1;
            end

            case (state)
                ADP_IDLE: begin
                    if (i_re || i_we) state <= ADP_BEGIN;
                end

                ADP_BEGIN: begin
                    req_word <= addr_word;
                    req_tag  <= addr_tag;

                    if (i_re && line_hit) begin
                        // Held line answers it; the controller never hears
                        // about this access.
                        rdata_latched <= line_data[{addr_word, 5'b0} +: 32];
                        state         <= ADP_PRESENT;
                    end else if (i_we) begin
                        // Drop the line rather than merging the write into
                        // it: keeping the copy would have to reproduce the
                        // byte-lane select memory applies, and this path
                        // sits in the bus decode cone of a design bounded
                        // by placement, where that network costs more than
                        // the re-fetch it saves.
                        if (line_hit) line_valid <= 1'b0;
                        if (i_req_ready) state <= ADP_PRESENT;
                    end else if (i_re) begin
                        if (i_req_ready) state <= ADP_WAIT_RSP;
                    end else begin
                        // The master dropped its strobe before anything was
                        // pushed — the PRESENT->BEGIN fast path enters this
                        // state on a held strobe that turns out to be the
                        // tail of the access just completed, not a new one.
                        state <= ADP_IDLE;
                    end
                end

                ADP_WAIT_RSP: begin
                    if (i_rsp_valid) begin
                        rdata_latched <= rsp_word;
                        state         <= ADP_PRESENT;
                    end
                end

                ADP_PRESENT: begin
                    if (i_re || i_we) state <= ADP_BEGIN;
                    else              state <= ADP_IDLE;
                end

                default: state <= ADP_IDLE;
            endcase
        end
    end

`ifdef SIMULATION
    // A response only ever arrives for a read this adapter is waiting
    // on: nothing else is ever outstanding.
    always_ff @(posedge i_clk)
        if (!i_rst && i_rsp_valid && state != ADP_WAIT_RSP)
            $error("sdram_bus_adapter: response outside ADP_WAIT_RSP");
`endif

endmodule
