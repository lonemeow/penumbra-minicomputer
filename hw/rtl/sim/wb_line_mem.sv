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
// A write takes effect when accepted, under its byte selects; a read
// takes its line when accepted, so completion order is request order by
// construction. Storage is LINES lines, the address wrapping onto them;
// it starts zeroed and keeps its contents through reset, as DRAM does.

module wb_line_mem #(
    parameter int ADDR_BITS = 25,
    parameter int AUX_WIDTH = 4,
    parameter int LINES     = 4096
) (
    input  logic                 i_clk,
    input  logic                 i_rst,
    input  logic                 i_cyc,
    input  logic                 i_stb,
    input  logic                 i_we,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [ADDR_BITS-1:0] i_addr,      // above the storage: wraps
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [127:0]         i_data,
    input  logic [15:0]          i_sel,
    input  logic [AUX_WIDTH-1:0] i_aux,
    output logic                 o_stall,
    output logic                 o_ack,
    output logic [127:0]         o_data,
    output logic [AUX_WIDTH-1:0] o_aux
);
    localparam int DEPTH = 8;               // requests in flight
    localparam int IDX_W = $clog2(LINES);
    localparam int Q_W   = $clog2(DEPTH);

    logic [127:0]         mem    [LINES];
    logic [127:0]         q_data [DEPTH];
    logic [AUX_WIDTH-1:0] q_aux  [DEPTH];
    logic [31:0]          q_due  [DEPTH];   // the cycle its ack may go out
    logic [Q_W-1:0]       head_q, tail_q;
    logic [Q_W:0]         count_q;
    logic [31:0]          now_q;
    logic [7:0]           stall_left_q;     // cycles the port still stalls
    logic [11:0]          refresh_in_q;     // cycles to the next long stall

    logic             accept, complete;
    logic [IDX_W-1:0] idx;

    assign idx      = i_addr[IDX_W-1:0];
    assign accept   = i_cyc && i_stb && !o_stall;
    assign complete = (count_q != '0) && (q_due[head_q] <= now_q);
    assign o_stall  = i_rst || (stall_left_q != '0) || (count_q == (Q_W+1)'(DEPTH));

    initial begin
        for (int i = 0; i < LINES; i++)
            mem[i] = '0;
    end

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
                if (i_we) begin
                    for (int b = 0; b < 16; b++)
                        if (i_sel[b])
                            mem[idx][8*b +: 8] <= i_data[8*b +: 8];
                end
                q_data[tail_q] <= i_we ? '0 : mem[idx];
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
