// penumbra4_bus_master — runs one port transaction on the external bus.
//
// A line read is four word reads in address order; a word read or a word
// write is exactly one access, as itself
// (doc/internals/penumbra3/uncore-port.md, "The three transactions"). Each
// access is the bus's sync handshake (doc/hardware/bus-protocol.md): the
// strobe and fields are held from flops through the ack cycle, and a strobe
// still up in the cycle after an ack is the line's next word.
//
// o_done pulses the cycle after the last ack, so a transaction never ends in
// the cycle it starts; o_idle is low through that cycle too, so a request
// still held while its done is up is not taken again.

module penumbra4_bus_master
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic        i_start,        // only while o_idle
    input  uncore_req_t i_req,
    output logic        o_idle,
    output logic        o_done,
    output logic        o_fault,        // valid with o_done
    output line_t       o_line,         // valid with o_done; a word read's word is word 0

    output paddr_t      o_bus_addr,
    output word_t       o_bus_wdata,
    output logic [3:0]  o_bus_byte_en,
    output logic        o_bus_re,
    output logic        o_bus_we,
    input  word_t       i_bus_rdata,
    input  logic        i_bus_ack,
    input  logic        i_bus_fault     // valid with i_bus_ack
);

    typedef enum logic {
        IDLE,
        ACCESS
    } state_e;

    state_e      state_q, state_d;
    uncore_req_t req_q, req_d;
    paddr_t      addr_q, addr_d;
    line_word_t  word_q, word_d;        // the line word the access in flight reads
    line_t       line_q, line_d;
    logic        fault_q, fault_d;
    logic        done_q, done_d;

    logic        last_word;

    assign last_word = !is_line_read(req_q) || (word_q == line_word_t'(LINE_WORDS - 1));

    always_comb begin
        state_d = state_q;
        req_d   = req_q;
        addr_d  = addr_q;
        word_d  = word_q;
        line_d  = line_q;
        fault_d = fault_q;
        done_d  = 1'b0;

        unique case (state_q)
            IDLE: begin
                if (i_start) begin
                    state_d = ACCESS;
                    req_d   = i_req;
                    line_d  = '0;
                    fault_d = 1'b0;
                    word_d  = '0;
                    if (is_line_read(i_req)) begin
                        addr_d = {i_req.addr[31:4], 4'b0};
                    end else begin
                        addr_d = i_req.addr;
                    end
                end
            end
            ACCESS: begin
                if (i_bus_ack) begin
                    line_d[word_q] = i_bus_rdata;
                    fault_d        = fault_q || i_bus_fault;
                    if (last_word) begin
                        state_d = IDLE;
                        done_d  = 1'b1;
                    end else begin
                        addr_d = addr_q + paddr_t'(4);
                        word_d = word_q + 1'b1;
                    end
                end
            end
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q <= IDLE;
            done_q  <= 1'b0;
        end else begin
            state_q <= state_d;
            done_q  <= done_d;
        end
    end

    always_ff @(posedge i_clk) begin
        req_q   <= req_d;
        addr_q  <= addr_d;
        word_q  <= word_d;
        line_q  <= line_d;
        fault_q <= fault_d;
    end

    assign o_idle        = (state_q == IDLE) && !done_q;
    assign o_done        = done_q;
    assign o_fault       = fault_q;
    assign o_line        = line_q;

    // A word access passes its lanes to the bus as they are; a line's
    // words are read whole.
    always_comb begin
        if (is_line_read(req_q)) begin
            o_bus_byte_en = 4'b1111;
        end else begin
            o_bus_byte_en = req_q.byte_en;
        end
    end

    assign o_bus_addr    = addr_q;
    assign o_bus_wdata   = req_q.wdata;
    assign o_bus_re      = (state_q == ACCESS) && !req_q.we;
    assign o_bus_we      = (state_q == ACCESS) && req_q.we;

endmodule
