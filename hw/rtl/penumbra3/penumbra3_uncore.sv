// penumbra3_uncore -- the gen3 uncore with no L2: both L1s' requests onto
// the external bus.
//
// The two L1s are the requesters on the core-to-uncore port
// (doc/internals/penumbra3/uncore-port.md). With no L2 the uncore turns
// each transaction into bus accesses through penumbra3_bus_master, one
// transaction at a time (doc/internals/penumbra3/machine.md, "The uncore
// with no L2"):
//
//   line read        the line's four words, from the line's address
//   word read        one read, its address and byte lanes as they are
//   word write       one write, likewise
//   cacheable write  one write, likewise
//
// When both sides wait, the data side goes first. The chosen request goes
// to the bus master in the cycle the uncore sees it, through the select
// ahead of the bus master's request register. The responses come from
// registers: each side's done, the fault and the store fault from the
// uncore's, the line from the bus master's buffer, which nothing writes
// until the next transaction. A transaction is in flight from its issue
// through its done, while its requester still holds the request, so no
// request is taken twice.
//
// A cacheable write's done carries no fault: the bus faulting the write
// shows as a store fault with the write's address, in the done's cycle.
// Nothing is posted, so a write's done follows the bus taking it.
//
// One transaction, cycle by cycle; i is the issue, m the bus master's
// completion:
//
//   i        a request is up and none is in flight: the bus master takes it
//   i+1..m   the bus accesses; the bus master completes in m
//   m+1      the side's done, with o_rsp_fault, o_rsp_line and the store
//            fault. Its request is still up, and nothing issues
//   m+2      a request up from here is a new one, and may issue
module penumbra3_uncore
    import penumbra3_pkg::*;
#(
    parameter  int LINE_WORDS  = UNCORE_LINE_WORDS,
    localparam int LINE_BYTES  = LINE_WORDS * 4,
    localparam int OFFSET_BITS = $clog2(LINE_BYTES)
) (
    input  logic                         i_clk,
    input  logic                         i_rst,

    // -- Fetch side: the L1-I's request ----------------------------
    input  logic                         i_ireq_valid,
    input  logic                         i_ireq_cacheable,
    input  logic [31:0]                  i_ireq_addr,
    output logic                         o_irsp_done,

    // -- Data side: the L1-D's request -----------------------------
    input  logic                         i_dreq_valid,
    input  logic                         i_dreq_cacheable,
    input  logic [31:0]                  i_dreq_addr,
    input  logic                         i_dreq_we,
    input  logic [3:0]                   i_dreq_byte_en,
    input  logic [31:0]                  i_dreq_wdata,
    output logic                         o_drsp_done,

    // -- The response both sides share -----------------------------
    output logic                         o_rsp_fault,        // valid with either done
    output logic [LINE_WORDS-1:0][31:0]  o_rsp_line,         // valid with a read's done
    output logic                         o_store_fault,      // a cacheable write faulted
    output logic [31:0]                  o_store_fault_addr, // its address

    // -- External bus (master side) --------------------------------
    output logic [31:0]                  o_bus_addr,
    output logic [31:0]                  o_bus_wdata,
    output logic [3:0]                   o_bus_byte_en,
    output logic                         o_bus_re,
    output logic                         o_bus_we,
    input  logic [31:0]                  i_bus_rdata,
    input  logic                         i_bus_ack,
    input  logic                         i_bus_fault        // valid with i_bus_ack
);

    // inflight_q: a transaction is in flight, from the cycle after its issue
    //             through its done.
    // dside_q:    it is the data side's.
    // cwrite_q:   it is a cacheable write.
    // addr_q:     its address, as the request gave it.
    logic        inflight_q, dside_q, cwrite_q;
    logic [31:0] addr_q;

    // -- The select: the data side first ---------------------------
    logic issue;
    assign issue = ~inflight_q & (i_dreq_valid | i_ireq_valid);

    // The chosen request's transaction. A fetch reads whole words.
    logic                        req_we, req_cacheable, req_line;
    logic [31:0]                 req_addr, req_wdata;
    logic [3:0]                  req_byte_en;
    always_comb begin
        if (i_dreq_valid) begin
            req_we        = i_dreq_we;
            req_cacheable = i_dreq_cacheable;
            req_addr      = i_dreq_addr;
            req_byte_en   = i_dreq_byte_en;
            req_wdata     = i_dreq_wdata;
        end else begin
            req_we        = 1'b0;
            req_cacheable = i_ireq_cacheable;
            req_addr      = i_ireq_addr;
            req_byte_en   = 4'hF;
            req_wdata     = 32'b0;
        end
        req_line = req_cacheable & ~req_we;
    end

    // A line read starts at its line; every other access goes as it is.
    logic [31:0]                 bm_addr;
    logic [LINE_WORDS-1:0][31:0] bm_wline;
    always_comb begin
        if (req_line) bm_addr = {req_addr[31:OFFSET_BITS], {OFFSET_BITS{1'b0}}};
        else          bm_addr = req_addr;
        bm_wline    = '0;
        bm_wline[0] = req_wdata;
    end

    // -- The bus master --------------------------------------------
    logic                        bm_ready, bm_done, bm_fault;
    logic [LINE_WORDS-1:0][31:0] bm_line;

    penumbra3_bus_master #(
        .LINE_BYTES (LINE_BYTES)
    ) u_bus_master (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_req_valid   (issue),
        .o_req_ready   (bm_ready),
        .i_req_we      (req_we),
        .i_req_line    (req_line),
        .i_req_addr    (bm_addr),
        .i_req_byte_en (req_byte_en),
        .i_req_wline   (bm_wline),
        .o_rsp_valid   (bm_done),
        .o_rsp_rline   (bm_line),
        .o_rsp_fault   (bm_fault),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    // -- The transaction in flight, and its response ---------------
    logic idone_q, ddone_q, rsp_fault_q, store_fault_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            inflight_q    <= 1'b0;
            idone_q       <= 1'b0;
            ddone_q       <= 1'b0;
            store_fault_q <= 1'b0;
        end else begin
            if (issue)                  inflight_q <= 1'b1;
            else if (idone_q | ddone_q) inflight_q <= 1'b0;

            idone_q       <= bm_done & ~dside_q;
            ddone_q       <= bm_done &  dside_q;
            store_fault_q <= bm_done & cwrite_q & bm_fault;
        end
    end

    always_ff @(posedge i_clk) begin
        if (issue) begin
            dside_q  <= i_dreq_valid;
            cwrite_q <= i_dreq_valid & i_dreq_we & i_dreq_cacheable;
            addr_q   <= req_addr;
        end
        if (bm_done) rsp_fault_q <= bm_fault & ~cwrite_q;
    end

    assign o_irsp_done        = idone_q;
    assign o_drsp_done        = ddone_q;
    assign o_rsp_fault        = rsp_fault_q;
    assign o_rsp_line         = bm_line;
    assign o_store_fault      = store_fault_q;
    assign o_store_fault_addr = addr_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The bus master is idle whenever nothing is in flight.
    assert property (@(posedge i_clk) disable iff (i_rst)
        issue |-> bm_ready)
        else $error("penumbra3_uncore: issued to a busy bus master");
    // The bus master completes only the transaction in flight.
    assert property (@(posedge i_clk) disable iff (i_rst)
        bm_done |-> inflight_q)
        else $error("penumbra3_uncore: a completion with nothing in flight");
    // A requester holds its request through its done
    // (doc/internals/penumbra3/uncore-port.md, "Handshake").
    assert property (@(posedge i_clk) disable iff (i_rst)
        inflight_q |-> (dside_q ? i_dreq_valid : i_ireq_valid))
        else $error("penumbra3_uncore: a request dropped before its done");

endmodule
