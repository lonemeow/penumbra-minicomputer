// penumbra3_branch_predictor -- the fetch-domain branch predictor.
//
// Beside IF2, it predicts each slot from the slot's own word: whether fetch
// goes on to a target behind it, and to which. Each L1-I way's word is
// pre-decoded straight off the block RAM (penumbra3_predecode), the direction
// table's counter for the slot arrives beside it, read with the fetch at IF1,
// and the return stack's top comes off registers, so the L1-I's way hit only
// selects a finished prediction. It only predicts: IF1 steers fetch by the
// prediction, and a taken transfer predicted here costs one cycle
// (doc/internals/penumbra3/branch-prediction.md, "Design").
//
// The prediction waits on no verdict but the hit. The translation fault and
// cacheability stay off it: a faulting slot traps and a missing one is
// replayed, so a prediction for either costs at most the fetch it discards.
// Nothing here knows whether IF2 holds a slot at all: the prediction is for
// whatever the L1-I last looked up, and IF1, which owns the slot's valid bit,
// acts on it only for a live slot. A slot taking a replayed word predicts
// from its own lookup, and only if that lookup hit.
//
// Each slot enqueues with its prediction (prediction_t), which EX verifies.
// EX steps each conditional branch's counter toward its outcome, and the
// table takes the new counter from a register, a cycle after EX resolves it.
//
// The return stack moves a cycle after a call or a return enqueues, off the
// prediction's path. It moves on the enqueued bundle, not on the pre-decode,
// so a call or return that took a replayed word with no hit of its own still
// keeps the stack in step with the program.

module penumbra3_branch_predictor
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  int WAYS           = 2,     // the L1-I's
    parameter  int DIR_ENTRIES    = 1024,
    parameter  int RAS_DEPTH      = 8,
    localparam int DIR_INDEX_BITS = $clog2(DIR_ENTRIES)
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── IF1's launch: the direction table reads beside the L1-I ──
    input  logic                  i_fetch_en,
    input  logic [31:0]           i_fetch_addr,

    // ── IF2's slot and its L1-I lookup ───────────────────────────
    // The L1-I reads every way of the slot's set at once: i_way_word is each
    // way's word, i_way_hit which way, if any, holds the slot's line.
    input  logic [31:0]           i_slot_pc,
    input  logic [WAYS-1:0]       i_way_hit,      // one-hot
    input  logic [WAYS-1:0][31:0] i_way_word,

    // ── The slot as it enqueues ──────────────────────────────────
    input  logic                  i_enq_fire,
    input  logic                  i_enq_fault,
    /* verilator lint_off UNUSEDSIGNAL */
    input  ctrl_bundle_t          i_enq_bundle,   // only its class and link bits
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0]           i_enq_next_pc,

    // ── To IF1 and the fetch buffer ──────────────────────────────
    output prediction_t           o_prediction,   // the slot's: IF1 steers by it, EX verifies it

    // ── Training (from EX) ───────────────────────────────────────
    input  logic                  i_train,        // a conditional branch resolved
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]           i_train_pc,     // only the index bits select
    /* verilator lint_on UNUSEDSIGNAL */
    input  dir_counter_t          i_train_counter // stepped toward its outcome
);

    // ── Direction table ──────────────────────────────────────────
    // Training is registered here, so the table's write port starts from
    // flops and EX's resolution reaches no further than this register.
    logic                      train_q;
    logic [DIR_INDEX_BITS-1:0] train_index_q;
    dir_counter_t              train_counter_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) train_q <= 1'b0;
        else       train_q <= i_train;
        train_index_q   <= i_train_pc[2 +: DIR_INDEX_BITS];
        train_counter_q <= i_train_counter;
    end

    dir_counter_t counter;   // the slot's, beside IF2's word
    penumbra3_direction_table #(
        .ENTRIES (DIR_ENTRIES)
    ) u_direction (
        .i_clk           (i_clk),
        .i_read_en       (i_fetch_en),
        .i_fetch_pc      (i_fetch_addr),
        .o_counter       (counter),
        .i_train_en      (train_q),
        .i_train_index   (train_index_q),
        .i_train_counter (train_counter_q)
    );

    // ── Return stack ─────────────────────────────────────────────
    // A call is BL or JALR (a transfer that links); a return is JMP R13. A
    // faulting slot traps instead of transferring, so it moves nothing.
    logic is_call, is_return;
    assign is_call   = (i_enq_bundle.op_class == OPC_BRANCH || i_enq_bundle.op_class == OPC_JMP)
                     && i_enq_bundle.dst_we;
    assign is_return = i_enq_bundle.op_class == OPC_JMP && !i_enq_bundle.dst_we
                     && i_enq_bundle.src_a_sel == REG_LR;

    logic        push_q, pop_q;
    logic [31:0] push_addr_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            push_q <= 1'b0;
            pop_q  <= 1'b0;
        end else begin
            push_q <= i_enq_fire && !i_enq_fault && is_call;
            pop_q  <= i_enq_fire && !i_enq_fault && is_return;
        end
        push_addr_q <= i_enq_next_pc;
    end

    logic [31:0] return_top;
    penumbra3_return_stack #(
        .DEPTH (RAS_DEPTH)
    ) u_return (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_push      (push_q),
        .i_push_addr (push_addr_q),
        .i_pop       (pop_q),
        .o_top       (return_top)
    );

    // ── Pre-decode, per way ──────────────────────────────────────
    // The stack moves on the bundle, so the pre-decode's call output has no
    // reader here.
    logic [WAYS-1:0]       way_direct, way_conditional, way_return;
    logic [WAYS-1:0][31:0] way_direct_target;
    for (genvar w = 0; w < WAYS; w++) begin : g_predecode
        /* verilator lint_off PINCONNECTEMPTY */
        penumbra3_predecode u_predecode (
            .i_word        (i_way_word[w]),
            .i_pc          (i_slot_pc),
            .o_direct      (way_direct[w]),
            .o_conditional (way_conditional[w]),
            .o_call        (),
            .o_return      (way_return[w]),
            .o_target      (way_direct_target[w])
        );
        /* verilator lint_on PINCONNECTEMPTY */
    end

    // ── The prediction ───────────────────────────────────────────
    logic [WAYS-1:0]       way_taken;
    logic [WAYS-1:0][31:0] way_target;

    for (genvar w = 0; w < WAYS; w++) begin : g_predict
        always_comb begin
            way_taken[w] = 0;
            way_target[w] = 0;
            if (way_return[w]) begin
                way_taken[w]  = 1'b1;
                way_target[w] = return_top;
            end else if (way_direct[w]) begin
                if (way_conditional[w]) begin
                    way_taken[w]  = dir_counter_taken(counter);
                    way_target[w] = way_direct_target[w];
                end else begin
                    way_taken[w]  = 1'b1;
                    way_target[w] = way_direct_target[w];
                end
            end
        end
    end

    logic        taken;
    logic [31:0] target;
    always_comb begin
        taken  = 0;
        target = 0;
        for (int w = 0; w < WAYS; w++) begin
            taken  |= i_way_hit[w] & way_taken[w];
            target |= {32{i_way_hit[w]}} & way_target[w];
        end
    end

    assign o_prediction = '{taken: taken, target: target, counter: counter};

endmodule
