// Penumbra Bus response fabric — merges the slaves' responses into the
// master's (doc/hardware/bus-protocol.md, "The response fabric").
//
// The request half of the bus is a broadcast: every slave sees the
// address and strobes and claims an access by its own decode. This
// module is the response half — the FPGA's stand-in for a backplane's
// wired-OR acknowledge line, tri-stated data lines and bus timer — and
// needs no knowledge of which slaves exist.
//
//   ack    the plain OR of the slaves' acks. A slave acknowledges only an
//          access it claimed, so gating by the live select would only put
//          the address decode back on the response path.
//   rdata  each slave's read data gated by its own ack, so no read data is
//          ever chosen by the live address.
//   fault  the watchdog's: an access nothing acknowledges within
//          TIMEOUT_CYCLES is acknowledged here, with the fault. That is
//          how an absent device is reported — and a stuck one.

module bus_response_fabric #(
    parameter int NUM_SLAVES     = 1,
    // Well above the slowest legitimate access — the SDRAM on a refresh
    // and a row conflict, or an async-bus peripheral at about 1 us — so
    // the watchdog fires only when no device answers at all.
    parameter int TIMEOUT_CYCLES = 256
) (
    input  logic                        i_clk,
    input  logic                        i_rst,

    // ── Master strobes (the broadcast request half) ────────
    input  logic                        i_re,
    input  logic                        i_we,

    // ── Per-slave responses ────────────────────────────────
    input  logic [NUM_SLAVES-1:0]       i_sel,     // slave's own decode; checked for overlap only
    input  logic [NUM_SLAVES-1:0]       i_ack,
    input  logic [NUM_SLAVES-1:0][31:0] i_rdata,   // valid in the slave's ack cycle

    // ── Merged response to the master ──────────────────────
    output logic                        o_ack,
    output logic [31:0]                 o_rdata,
    output logic                        o_fault    // valid with o_ack
);

    if (TIMEOUT_CYCLES < 2) begin : chk_timeout
        $error("TIMEOUT_CYCLES must be at least 2");
    end

    localparam int WAIT_W = $clog2(TIMEOUT_CYCLES);

    logic slave_ack;
    assign slave_ack = |i_ack;

    // ── Watchdog ───────────────────────────────────────────
    // An access is waiting from its presentation until a slave, or the
    // watchdog itself, acknowledges it. The count restarts with every
    // access, so a strobe held past an ack starts a fresh wait.
    logic              waiting;
    logic              at_deadline;    // the access has used up its allowed wait
    logic [WAIT_W-1:0] waited_q;       // cycles the current access has waited
    logic [WAIT_W-1:0] waited_d;
    logic              timeout_q;
    logic              timeout_d;

    assign waiting     = (i_re | i_we) & ~slave_ack & ~timeout_q;
    assign at_deadline = (waited_q == WAIT_W'(TIMEOUT_CYCLES - 1));
    assign timeout_d   = waiting & at_deadline;
    assign waited_d    = (waiting & ~at_deadline) ? waited_q + 1'b1 : '0;

    always_ff @(posedge i_clk)
        if (i_rst) begin
            waited_q  <= '0;
            timeout_q <= 1'b0;
        end else begin
            waited_q  <= waited_d;
            timeout_q <= timeout_d;
        end

    // A slave that answers in the cycle the watchdog fires wins: its read
    // data is valid and its write has committed, so the access completes
    // without a fault.
    assign o_ack   = slave_ack || timeout_q;
    assign o_fault = !slave_ack && timeout_q;

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
        if (!i_rst && (i_re || i_we))
            assert ($onehot0(i_sel))
                else $error("bus_response_fabric: overlapping device decode (sel=%b)", i_sel);
    // synthesis translate_on

endmodule
