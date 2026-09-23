// Simple bus device handshake (doc/hardware/bus-protocol.md, "Slave
// obligations").
//
// A simple device resolves every access in one cycle: it does the
// access's work in its first cycle — commits a write, or captures read
// data and consumes it where the read pops a FIFO — and this cell
// answers in the next. The master holds the strobe through the ack
// cycle, so the bare strobe is high for two cycles per access; o_read
// and o_write are high for one, and the device keys every side effect
// on them.
//
//   cycle      strobe   o_read/o_write   o_ack
//   present      1            1            0     the device acts
//   ack          1            0            1     rdata presented
//   next         x            x            0     a held strobe starts
//                                                a new access
//
// A device whose latency varies (the SDRAM adapter, a sequenced
// register window) decides its own ack under the same rules.

module bus_device_simple (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_re,        // qualified by the device's select
    input  logic i_we,
    output logic o_read,      // a read starts this cycle: act on it now
    output logic o_write,     // a write starts this cycle: commit it now
    output logic o_ack
);

    logic ack_q;
    logic first;

    // The strobe seen in the ack cycle belongs to the access being
    // completed; in the cycle after, a held strobe is a new access.
    assign first   = (i_re | i_we) & ~ack_q;
    assign o_read  = i_re & first;
    assign o_write = i_we & first;
    assign o_ack   = ack_q;

    always_ff @(posedge i_clk)
        if (i_rst) ack_q <= 1'b0;
        else       ack_q <= first;

    assert property (@(posedge i_clk) disable iff (i_rst)
        ack_q |-> (i_re | i_we))
        else $error("bus_device_simple: strobe dropped before its ack");

endmodule
