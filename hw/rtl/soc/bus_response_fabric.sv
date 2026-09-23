// Penumbra Bus response fabric — merges the slaves' responses into the
// master's (doc/hardware/bus-protocol.md, "The response fabric").
//
// The request half of the bus is a broadcast: every slave sees the
// address and strobes and claims an access by its own decode, its
// select. This module is the response half — the FPGA's stand-in for a
// backplane's wired-OR acknowledge line and tri-stated data lines — plus
// the no-device responder, the one piece that needs the selects.
//
//   ack    the plain OR of the slaves' acks. A slave acknowledges only an
//          access it claimed, so gating by the live select would only put
//          the address decode back on the response path.
//   rdata  each slave's read data gated by its own ack, so no read data is
//          ever chosen by the live address.
//   fault  the no-device responder's: an access no select claims is
//          acknowledged here, with the fault, in its presentation cycle.

module bus_response_fabric #(
    parameter int NUM_SLAVES = 1
) (
    input  logic                        i_clk,
    input  logic                        i_rst,

    // ── Master strobes (the broadcast request half) ────────
    input  logic                        i_re,
    input  logic                        i_we,

    // ── Per-slave responses ────────────────────────────────
    input  logic [NUM_SLAVES-1:0]       i_sel,     // slave claims the presented address
    input  logic [NUM_SLAVES-1:0]       i_ack,
    input  logic [NUM_SLAVES-1:0][31:0] i_rdata,   // valid in the slave's ack cycle

    // ── Merged response to the master ──────────────────────
    output logic                        o_ack,
    output logic [31:0]                 o_rdata,
    output logic                        o_fault    // valid with o_ack
);

    logic strobe;
    assign strobe = i_re | i_we;

    // No-device responder: a slave like any other, whose select is the
    // OR-complement of every other select.
    logic nodev_ack;
    assign nodev_ack = strobe & ~|i_sel;

    assign o_ack   = nodev_ack | (|i_ack);
    assign o_fault = nodev_ack;

    always_comb begin
        o_rdata = '0;
        for (int s = 0; s < NUM_SLAVES; s++)
            if (i_ack[s])
                o_rdata = o_rdata | i_rdata[s];
    end

    // synthesis translate_off
    // Disjoint selects are structural for fixed decodes and a
    // software-held property for autoconfigured ones, whose bases are
    // assigned at run time. Two slaves claiming one access would merge
    // their read data silently.
    always_ff @(posedge i_clk)
        if (!i_rst && strobe)
            assert ($onehot0(i_sel))
                else $error("bus_response_fabric: overlapping device decode (sel=%b)", i_sel);
    // synthesis translate_on

endmodule
