// Wishbone burst memory model: a pipelined-Wishbone slave of 16-byte lines
//
// Stands in, in simulation, for a DDR3 controller's memory port (the one
// nexysvideo_ddr3 exposes). A request is accepted on an edge where stb is
// high and stall low; each completes, in order, with a one-cycle ack
// carrying the line a read asked for and the aux the request brought.
// Dropping cyc cancels everything in flight, acks included.
//
// Stall and ack latency are random, within bounds a real controller
// shows, so a consumer that only works at one timing fails here:
//   - latency, acceptance to ack: 6-14 cycles, a controller's pipeline
//     and CAS latency; one request in eight also waits 20-40 cycles
//     more, behind a row miss or a refresh;
//   - stall: 0-3 cycles after each accepted request, and every 700-900
//     cycles a stretch of 30-60, as a refresh holds the port.
//
// Calibration comes first: for CALIB_CYCLES after reset the port stalls
// and o_calib_complete stays low, as a DDR3 controller's does while it
// trains its read and write timing. Only i_rst restarts it; dropping cyc
// does not.
//
// A write takes effect when accepted, under its byte selects; a read
// takes its line when accepted, so completion order is request order by
// construction. Storage covers the whole address range, so the model
// backs a full-size part: 512 MiB of host memory at the DDR3 part's 25
// burst-address bits. It starts zeroed and keeps its contents through
// reset, as DRAM does.

module wb_line_mem #(
    parameter int ADDR_BITS    = 25,
    parameter int AUX_WIDTH    = 4,
    parameter int CALIB_CYCLES = 0
) (
    input  logic                 i_clk,
    input  logic                 i_rst,
    input  logic                 i_cyc,
    input  logic                 i_stb,
    input  logic                 i_we,
    input  logic [ADDR_BITS-1:0] i_addr,
    input  logic [127:0]         i_data,
    input  logic [15:0]          i_sel,
    input  logic [AUX_WIDTH-1:0] i_aux,
    output logic                 o_stall,
    output logic                 o_ack,
    output logic [127:0]         o_data,
    output logic [AUX_WIDTH-1:0] o_aux,
    output logic                 o_calib_complete
);
    localparam int DEPTH = 8;               // requests in flight
    localparam int Q_W   = $clog2(DEPTH);
    localparam int LINES = 1 << ADDR_BITS;

    logic [127:0]         mem    [LINES];
    // The model starts uncalibrated, as a controller does when the FPGA
    // configures, so nothing downstream sees it ready before its first
    // reset. A declaration initializer rather than an initial block,
    // since a variable an always_ff writes may have no other writer.
    /* verilator lint_off PROCASSINIT */
    logic [31:0]          calib_left_q = 32'(CALIB_CYCLES);   // cycles until calibrated
    /* verilator lint_on PROCASSINIT */
    logic [127:0]         q_data [DEPTH];
    logic [AUX_WIDTH-1:0] q_aux  [DEPTH];
    logic [31:0]          q_due  [DEPTH];   // the cycle its ack may go out
    logic [Q_W-1:0]       head_q, tail_q;
    logic [Q_W:0]         count_q;
    logic [31:0]          now_q;
    logic [7:0]           stall_left_q;     // cycles the port still stalls
    logic [11:0]          refresh_in_q;     // cycles to the next long stall

    logic         accept, complete;
    logic [127:0] sel_bits;                 // i_sel, one bit per data bit

    assign o_calib_complete = (calib_left_q == '0);
    assign accept           = i_cyc && i_stb && !o_stall;
    assign complete         = (count_q != '0) && (q_due[head_q] <= now_q);
    assign o_stall          = i_rst || !o_calib_complete || (stall_left_q != '0)
                           || (count_q == (Q_W+1)'(DEPTH));

    always_ff @(posedge i_clk) begin
        if (i_rst)
            calib_left_q <= 32'(CALIB_CYCLES);
        else if (calib_left_q != '0)
            calib_left_q <= calib_left_q - 32'd1;
    end

    always_comb
        for (int b = 0; b < 16; b++)
            sel_bits[8*b +: 8] = {8{i_sel[b]}};

    // The storage is zeroed at the start of simulation and written by a
    // plain always block, which unlike an always_ff may share a variable
    // with an initial block. (A declaration initializer would do instead,
    // but the simulator expands one element by element, all 2^25 of them,
    // at build time.) A write is one nonblocking write of the whole line,
    // merged under the selects: in the simulator that gets a delay slot
    // of its own, where a loop of byte-lane writes gets a shadow copy of
    // the entire array on every clock.
    initial
        for (int i = 0; i < LINES; i++)
            mem[i] = '0;

    always @(posedge i_clk)
        if (accept && i_we)
            mem[i_addr] <= (mem[i_addr] & ~sel_bits) | (i_data & sel_bits);

    function automatic logic [31:0] latency();
        logic [31:0] slow;
        slow = ($urandom_range(7, 0) == 0) ? 32'($urandom_range(40, 20)) : 32'd0;
        return slow + 32'($urandom_range(14, 6));
    endfunction

    always_ff @(posedge i_clk) begin
        o_ack <= 1'b0;
        now_q <= now_q + 32'd1;
        if (i_rst || !i_cyc) begin
            head_q       <= '0;
            tail_q       <= '0;
            count_q      <= '0;
            stall_left_q <= '0;
            refresh_in_q <= 12'($urandom_range(900, 700));
        end else begin
            if (accept) begin
                q_data[tail_q] <= i_we ? '0 : mem[i_addr];
                q_aux[tail_q]  <= i_aux;
                q_due[tail_q]  <= now_q + latency();
                tail_q         <= tail_q + Q_W'(1);
            end
            if (complete) begin
                o_ack  <= 1'b1;
                o_data <= q_data[head_q];
                o_aux  <= q_aux[head_q];
                head_q <= head_q + Q_W'(1);
            end
            count_q <= count_q + (Q_W+1)'(accept) - (Q_W+1)'(complete);

            if (stall_left_q != '0)
                stall_left_q <= stall_left_q - 8'd1;
            else if (refresh_in_q == '0)
                stall_left_q <= 8'($urandom_range(60, 30));
            else if (accept)
                stall_left_q <= 8'($urandom_range(3, 0));
            refresh_in_q <= (refresh_in_q == '0) ? 12'($urandom_range(900, 700))
                                                 : refresh_in_q - 12'd1;
        end
    end

endmodule
