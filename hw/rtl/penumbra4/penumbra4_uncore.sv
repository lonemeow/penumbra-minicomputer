// penumbra4_uncore — serves the L1s' accesses from the external bus.
//
// The uncore end of the core-to-uncore port
// (doc/internals/penumbra3/uncore-port.md): a requester holds its request
// until a one-cycle done, and done, fault and line come from registers.
// The select starts one side's request on the bus master while it is idle,
// the data side's first when both wait, since its access is older than any
// instruction the fetch side is fetching. A side's done is the bus master's
// for the side it serves; fault and line are the bus master's, which that
// side owns in its done cycle.

module penumbra4_uncore
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── The fetch side of the core-to-uncore port ────────────────
    input  logic        i_fetch_valid,
    input  uncore_req_t i_fetch_req,
    output logic        o_fetch_done,

    // ── The data side ────────────────────────────────────────────
    input  logic        i_data_valid,
    input  uncore_req_t i_data_req,
    output logic        o_data_done,

    // ── The response both sides share ────────────────────────────
    output logic        o_fault,        // valid with either done
    output line_t       o_line,         // valid with a read's done

    // ── External bus ─────────────────────────────────────────────
    output paddr_t      o_bus_addr,
    output word_t       o_bus_wdata,
    output logic [3:0]  o_bus_byte_en,
    output logic        o_bus_re,
    output logic        o_bus_we,
    input  word_t       i_bus_rdata,
    input  logic        i_bus_ack,
    input  logic        i_bus_fault
);

    typedef enum logic {
        SIDE_FETCH,
        SIDE_DATA
    } side_e;

    logic        bus_idle;
    logic        bus_done_q;
    logic        start;
    side_e       side_q, side_d;
    uncore_req_t req;

    always_comb begin
        side_d = side_q;
        req    = i_fetch_req;
        start  = 1'b0;
        if (bus_idle) begin
            if (i_data_valid) begin
                start  = 1'b1;
                side_d = SIDE_DATA;
                req    = i_data_req;
            end else if (i_fetch_valid) begin
                start  = 1'b1;
                side_d = SIDE_FETCH;
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            side_q <= SIDE_FETCH;
        end else begin
            side_q <= side_d;
        end
    end

    assign o_fetch_done = bus_done_q && (side_q == SIDE_FETCH);
    assign o_data_done  = bus_done_q && (side_q == SIDE_DATA);

    penumbra4_bus_master u_bus_master (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_start       (start),
        .i_req         (req),
        .o_idle        (bus_idle),
        .o_done        (bus_done_q),
        .o_fault       (o_fault),
        .o_line        (o_line),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    // ── Contract ─────────────────────────────────────────────────
    // The port's handshake (doc/internals/penumbra3/uncore-port.md,
    // "Handshake"): a requester holds its request unchanged through its
    // done, a done answers a request up since an earlier cycle and lasts
    // one, and the two sides' dones never coincide.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_fetch_valid && !o_fetch_done) |=> (i_fetch_valid && $stable(i_fetch_req)))
        else $error("penumbra4_uncore: the fetch side changed or withdrew its request before its done");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_data_valid && !o_data_done) |=> (i_data_valid && $stable(i_data_req)))
        else $error("penumbra4_uncore: the data side changed or withdrew its request before its done");
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_fetch_done |-> (i_fetch_valid && $past(i_fetch_valid)))
        else $error("penumbra4_uncore: a fetch-side done with no request up before it");
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_data_done |-> (i_data_valid && $past(i_data_valid)))
        else $error("penumbra4_uncore: a data-side done with no request up before it");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_fetch_done || o_data_done) |=> !(o_fetch_done || o_data_done))
        else $error("penumbra4_uncore: a done longer than a cycle");
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_fetch_done && o_data_done))
        else $error("penumbra4_uncore: both sides' dones in one cycle");

endmodule
