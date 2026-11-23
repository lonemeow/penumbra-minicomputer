// penumbra4_uncore — serves the L1s' accesses from the external bus.
//
// The uncore end of the core-to-uncore port
// (doc/internals/penumbra3/uncore-port.md): a requester holds its request
// until a one-cycle done, and done, fault and line come from registers.
// It serves the fetch side, through the bus master.

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
    output logic        o_fault,        // valid with a done
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

    logic bus_idle;

    penumbra4_bus_master u_bus_master (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_start       (i_fetch_valid && bus_idle),
        .i_req         (i_fetch_req),
        .o_idle        (bus_idle),
        .o_done        (o_fetch_done),
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

endmodule
