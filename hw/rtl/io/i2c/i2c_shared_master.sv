// Penumbra I2C bus master for several consumers
//
// An i2c_master's command port takes one driver. This is that master
// with a command port for each of CONSUMERS drivers, which have it a
// transaction at a time: a consumer has the master from the command that
// carries its START to the response that ends the transaction, and then
// the next one asking has it. A bus with several devices behind it takes
// this where a bus with one consumer takes an i2c_master.
//
// Each port is i2c_master's own command port, signal for signal, and
// needs nothing more. A consumer that is not being served sees its
// command not taken yet, and waits as it would on a master that is
// busy. So a module written to drive an i2c_master drives a port here
// unchanged.
//
// Where a transaction ends is that command port's own contract. A
// response ends its transaction when any of these holds:
//
//   * the command it answers carried a STOP;
//   * that command was a write, and the response says the slave did not
//     acknowledge it (o_rsp_nack): the master has put a STOP of its own
//     behind the byte;
//   * the response reports a bus error (o_rsp_error): the master read a
//     line low that it had let go, and gave the command up.
//
// Behind such a response the master holds no transaction, and the next
// command needs a START. A consumer ends every transaction it begins.
//
// The consumers take turns. The next to be served is the first one
// asking, counting on from the one served last, so a consumer waits for
// one transaction of each of the others at most, whatever they do.
// Transactions of different consumers follow each other in any order: a
// consumer whose commands must not be parted makes them one transaction,
// with a repeated START.

module i2c_shared_master #(
    // The clock's rate and the bus's, as i2c_master takes them, and the
    // consumers sharing the master. All three are a board's own: no
    // defaults.
    parameter int CLK_HZ,
    parameter int SCL_HZ,
    parameter int CONSUMERS
)(
    input  logic                      i_clk,
    input  logic                      i_rst,

    // ── The consumers: a command port each, as i2c_master's ──
    // Bit n, or byte n, of each signal is consumer n's.
    input  logic [CONSUMERS-1:0]      i_cmd_valid,
    output logic [CONSUMERS-1:0]      o_cmd_ready,
    input  logic [CONSUMERS-1:0]      i_cmd_start,
    input  logic [CONSUMERS-1:0]      i_cmd_stop,
    input  logic [CONSUMERS-1:0]      i_cmd_read,
    input  logic [CONSUMERS-1:0][7:0] i_cmd_wdata,
    input  logic [CONSUMERS-1:0]      i_cmd_nack,
    output logic [CONSUMERS-1:0]      o_rsp_valid,
    output logic [7:0]                o_rsp_rdata,    // one for all: o_rsp_valid says whose
    output logic                      o_rsp_nack,
    output logic                      o_rsp_error,

    // ── Pins, as i2c_master's ────────────────────────────────
    output logic                      o_scl,
    input  logic                      i_scl,
    output logic                      o_sda,
    input  logic                      i_sda
);

    // One consumer drives an i2c_master itself.
    build_check #(
        .COND (CONSUMERS == 0 || CONSUMERS >= 2),
        .MSG  ("i2c_shared_master: CONSUMERS is less than 2: use an i2c_master")
    ) u_check_consumers ();

    // ── The master ───────────────────────────────────────────
    logic       master_cmd_valid, master_cmd_ready;
    logic       master_cmd_start, master_cmd_stop, master_cmd_read, master_cmd_nack;
    logic [7:0] master_cmd_wdata;
    logic       master_rsp_valid, master_rsp_nack, master_rsp_error;
    logic [7:0] master_rsp_rdata;

    i2c_master #(
        .CLK_HZ (CLK_HZ),
        .SCL_HZ (SCL_HZ)
    ) u_master (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_cmd_valid (master_cmd_valid),
        .o_cmd_ready (master_cmd_ready),
        .i_cmd_start (master_cmd_start),
        .i_cmd_stop  (master_cmd_stop),
        .i_cmd_read  (master_cmd_read),
        .i_cmd_wdata (master_cmd_wdata),
        .i_cmd_nack  (master_cmd_nack),
        .o_rsp_valid (master_rsp_valid),
        .o_rsp_rdata (master_rsp_rdata),
        .o_rsp_nack  (master_rsp_nack),
        .o_rsp_error (master_rsp_error),
        .o_scl       (o_scl),
        .i_scl       (i_scl),
        .o_sda       (o_sda),
        .i_sda       (i_sda)
    );

    localparam int OWNER_W = (CONSUMERS > 1) ? $clog2(CONSUMERS) : 1;

    typedef logic [OWNER_W-1:0] owner_t;
    localparam owner_t LAST_CONSUMER = owner_t'(CONSUMERS - 1);

    logic   serving_q;      // a consumer has the master, until its transaction ends
    owner_t owner_q;        // that consumer; between transactions, the one served last
    logic   inflight_q;     // the master holds a command, its response yet to come
    logic   stop_q;         // that command carried a STOP,
    logic   read_q;         //   and was a read

    // ── Whose turn ───────────────────────────────────────────
    // The choice is registered: a consumer is chosen in one cycle and
    // its commands reach the master from the next, so no path runs from
    // every consumer's port through the choice into the master.
    // The consumer `places` on from consumer `from`, round the ring.
    function automatic owner_t ring_step(input owner_t from, input int places);
        int n;
        n = int'(from) + places;
        if (n >= CONSUMERS)
            n = n - CONSUMERS;
        ring_step = owner_t'(n);
    endfunction

    logic   any_asking;
    owner_t next_owner;

    always_comb begin
        any_asking = |i_cmd_valid;
        next_owner = owner_q;
        // The farthest in line is looked at first, and the last served is
        // the farthest, so the nearest one asking has the last word.
        for (int places = CONSUMERS; places >= 1; places--) begin
            if (i_cmd_valid[ring_step(owner_q, places)])
                next_owner = ring_step(owner_q, places);
        end
    end

    // ── The transaction ──────────────────────────────────────
    // The response in hand ends its transaction: the master holds none
    // from here on.
    logic ends;
    assign ends = stop_q ||
        (!read_q && master_rsp_nack) ||
        master_rsp_error;

    // The master is ready again in the cycle it responds, and the owner
    // may have its next command up already. Held back until the response
    // is in, that command is never taken as part of a transaction the
    // response has just ended: it begins a new one, and waits its turn.
    logic offering;
    assign offering = serving_q && !inflight_q;

    logic cmd_take;
    assign cmd_take = master_cmd_valid && master_cmd_ready;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            serving_q  <= 1'b0;
            owner_q    <= LAST_CONSUMER;    // consumer 0 is first in line
            inflight_q <= 1'b0;
            stop_q     <= 1'b0;
            read_q     <= 1'b0;
        end else if (!serving_q) begin
            if (any_asking) begin
                serving_q <= 1'b1;
                owner_q   <= next_owner;
            end
        end else begin
            if (cmd_take) begin
                inflight_q <= 1'b1;
                stop_q     <= master_cmd_stop;
                read_q     <= master_cmd_read;
            end
            if (master_rsp_valid) begin
                inflight_q <= 1'b0;
                if (ends)
                    serving_q <= 1'b0;
            end
        end
    end

    // ── The owner's port, joined to the master's ─────────────
    assign master_cmd_valid = offering && i_cmd_valid[owner_q];
    assign master_cmd_start = i_cmd_start[owner_q];
    assign master_cmd_stop  = i_cmd_stop[owner_q];
    assign master_cmd_read  = i_cmd_read[owner_q];
    assign master_cmd_wdata = i_cmd_wdata[owner_q];
    assign master_cmd_nack  = i_cmd_nack[owner_q];

    always_comb begin
        o_cmd_ready = '0;
        o_rsp_valid = '0;
        if (serving_q) begin
            o_cmd_ready[owner_q] = offering && master_cmd_ready;
            o_rsp_valid[owner_q] = master_rsp_valid;
        end
    end

    assign o_rsp_rdata = master_rsp_rdata;
    assign o_rsp_nack  = master_rsp_nack;
    assign o_rsp_error = master_rsp_error;

    // synthesis translate_off
    // Every command the master answers was some consumer's.
    always_ff @(posedge i_clk) begin
        if (!i_rst && master_rsp_valid)
            assert (serving_q)
                else $error("i2c_shared_master: a response with no consumer being served");
    end
    // synthesis translate_on

endmodule
