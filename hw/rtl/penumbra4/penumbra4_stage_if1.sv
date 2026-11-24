// penumbra4_stage_if1 — IF1: chooses the address fetch reads next and
// launches its lookup.
//
// The one place that steers (doc/internals/penumbra4/structure.md,
// "Behaviours and the parts that carry them"). Its sources, highest first:
// WB's redirect, the vector read a trap's redirect starts, EX's
// misprediction, IF2's relaunch, and the next line. A redirect or a
// relaunch takes the cycle, launching nothing, and clears the fetch it
// launched a cycle before, which IF2 holds.
//
// It launches nothing while the L1-I is busy, so a relaunch that waits on an
// access goes out the cycle the access is done, and takes its result. A
// vector read is an access of the L1-I's with no lookup: requested once the
// L1-I is idle, its word is the handler's address.
//
// The output register is the fetch IF2 holds.

module penumbra4_stage_if1
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter vaddr_t RESET_PC = 32'hFFFF_0000
)(
    input  logic             i_clk,
    input  logic             i_rst,

    input  wb_redirect_t     i_wb_redirect,
    input  redirect_t        i_ex_redirect,
    input  relaunch_t        i_relaunch,
    input  logic             i_queue_room,

    input  logic             i_icache_busy,
    input  logic             i_icache_done,
    input  word_t            i_icache_word,

    output logic             o_launch,
    output vaddr_t           o_launch_vaddr,
    output logic             o_vector_req,
    output paddr_t           o_vector_addr,
    output fetch_slot_t      o_if2
);

    typedef enum logic [1:0] {
        FETCHING,
        VECTOR_REQUEST,    // waits for the L1-I to be idle
        VECTOR_READ        // waits for the vector's word
    } state_e;

    state_e      state_q, state_d;
    vaddr_t      pc_q, pc_d;
    logic        from_access_q, from_access_d;
    vector_t     vector_q, vector_d;
    fetch_slot_t if2_q, if2_d;

    always_comb begin
        state_d       = state_q;
        pc_d          = pc_q;
        from_access_d = from_access_q;
        vector_d      = vector_q;
        if2_d         = '0;
        o_launch      = 1'b0;
        o_vector_req  = 1'b0;

        if (i_wb_redirect.valid) begin
            from_access_d = 1'b0;
            if (i_wb_redirect.to_vector) begin
                state_d  = VECTOR_REQUEST;
                vector_d = i_wb_redirect.vector;
            end else begin
                state_d  = FETCHING;
                pc_d     = i_wb_redirect.target;
            end
        end else if (state_q == VECTOR_REQUEST) begin
            if (!i_icache_busy) begin
                o_vector_req = 1'b1;
                state_d      = VECTOR_READ;
            end
        end else if (state_q == VECTOR_READ) begin
            if (i_icache_done) begin
                state_d = FETCHING;
                pc_d    = vaddr_t'(i_icache_word);
            end
        end else if (i_ex_redirect.valid) begin
            pc_d          = i_ex_redirect.target;
            from_access_d = 1'b0;
        end else if (i_relaunch.valid) begin
            pc_d          = i_relaunch.pc;
            from_access_d = i_relaunch.from_access;
        end else if (!i_icache_busy && i_queue_room) begin
            o_launch      = 1'b1;
            if2_d.valid       = 1'b1;
            if2_d.pc          = pc_q;
            if2_d.from_access = from_access_q;
            pc_d          = {pc_q[31:4] + 28'd1, 4'b0};
            from_access_d = 1'b0;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q       <= FETCHING;
            pc_q          <= RESET_PC;
            from_access_q <= 1'b0;
            if2_q         <= '0;
        end else begin
            state_q       <= state_d;
            pc_q          <= pc_d;
            from_access_q <= from_access_d;
            if2_q         <= if2_d;
        end
    end

    always_ff @(posedge i_clk) begin
        vector_q <= vector_d;
    end

    assign o_launch_vaddr = pc_q;
    assign o_vector_addr  = paddr_t'({vector_q, 2'b00});
    assign o_if2          = if2_q;

endmodule
