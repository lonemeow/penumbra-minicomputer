// penumbra4_issue — ID: decides which ops issue and in which slots, and
// reads their registers.
//
// One op a cycle, from the queue's head into Y. An op issues once nothing
// in flight writes what it reads, since no result is forwarded: every value
// comes from the register file and the committed status. A system
// instruction issues alone into an empty pipeline, and nothing issues
// behind it until it has committed. A privileged op in user mode issues
// carrying VEC_PRIV.
//
// The ops in flight are read from the stage registers: this stage's own,
// and the later ones the core hands in, commit's landing effects last.
// Operands are captured here: a source of R15 takes the op's PC, RDSPR
// takes its register's value, and the flags are the committed ones.
//
// The output register is the pair execute works on.

module penumbra4_issue
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter int HEAD_WIDTH = 3,
    parameter int LATER      = 4     // stage registers after this one
)(
    input  logic                              i_clk,
    input  logic                              i_rst,

    input  op_t    [HEAD_WIDTH-1:0]           i_head,
    input  logic   [HEAD_WIDTH-1:0]           i_head_valid,
    output logic   [$clog2(HEAD_WIDTH+1)-1:0] o_taken,

    input  logic                              i_kill,
    input  logic                              i_hold,
    input  pair_t  [LATER-1:0]                i_in_flight,

    input  sr_t                               i_sr,
    input  word_t                             i_epc,
    input  word_t                             i_esr,
    input  word_t  [3:0]                      i_scratch,

    output phys_reg_t [3:0]                   o_read_phys,
    input  word_t     [3:0]                   i_read_value,

    output pair_t                             o_pair
);

    localparam int READ_PORT_Y_A = 2;
    localparam int READ_PORT_Y_B = 3;
    localparam int FLIGHT        = 2 * (LATER + 1);

    pair_t pair_q, pair_d;

    function automatic logic serializes(op_class_e op_class);
        return op_class inside {OPC_RDSPR, OPC_WRSPR, OPC_RDSYS, OPC_WRSYS,
                                OPC_IRQ_MASK, OPC_ERET};
    endfunction

    // ── The ops in flight ────────────────────────────────────────
    slot_t [FLIGHT-1:0] flight;
    logic               anything_in_flight;
    logic               serializer_in_flight;

    always_comb begin
        flight[0] = pair_q.x;
        flight[1] = pair_q.y;
        for (int s = 0; s < LATER; s++) begin
            flight[2 + 2 * s]     = i_in_flight[s].x;
            flight[2 + 2 * s + 1] = i_in_flight[s].y;
        end
        anything_in_flight   = 1'b0;
        serializer_in_flight = 1'b0;
        for (int f = 0; f < FLIGHT; f++) begin
            anything_in_flight   |= flight[f].valid;
            serializer_in_flight |= flight[f].valid && serializes(flight[f].op.op_class);
        end
    end

    // ── The head ─────────────────────────────────────────────────
    op_t  op;
    logic source_hazard;
    logic issue;

    always_comb begin
        op = i_head[0];
        if (op.privileged && !i_sr.s && !op.fault.valid) begin
            op.fault          = '0;
            op.fault.valid    = 1'b1;
            op.fault.vector   = VEC_PRIV;
            op.fault.mmu_type = FAULT_NONE;
        end
    end

    // A source of R15 reads the op's own PC, not the register file.
    function automatic logic reads_pc(reg_ref_t src);
        return src.en && src.index == REG_PC && !src.user;
    endfunction

    // Whether an op writes what a source reads. A source that reads the PC
    // reads no physical register, and no op writes the PC; it is ruled out
    // ahead of the compare, where its number would match SSP's.
    // A slot carries more than its destinations.
    /* verilator lint_off UNUSEDSIGNAL */
    function automatic logic writes_source(slot_t s, reg_ref_t src, logic supervisor);
        phys_reg_t read;
        if (!src.en || reads_pc(src)) begin
            return 1'b0;
        end
        read = phys_reg_of(src, supervisor);
        return (s.op.dst.en  && phys_reg_of(s.op.dst,  supervisor) == read)
            || (s.op.dst2.en && phys_reg_of(s.op.dst2, supervisor) == read);
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */

    // The head waits while an op in flight writes a register it reads, or
    // the flags when it reads them.
    always_comb begin
        source_hazard = 1'b0;
        for (int f = 0; f < FLIGHT; f++) begin
            if (flight[f].valid) begin
                source_hazard |= writes_source(flight[f], op.src_a, i_sr.s)
                              || writes_source(flight[f], op.src_b, i_sr.s)
                              || (op.reads_flags && flight[f].op.writes_flags);
            end
        end
    end

    assign issue = i_head_valid[0] && !i_kill && !i_hold
                && !serializer_in_flight
                && !(serializes(op.op_class) && anything_in_flight)
                && !source_hazard;

    // ── Operands ─────────────────────────────────────────────────
    function automatic word_t spr_value(spr_index_t spr);
        case (spr)
            SPR_ESR:  return i_esr;
            SPR_EPC:  return i_epc;
            SPR_SR:   return word_t'(i_sr);
            SPR_SCR0: return i_scratch[0];
            SPR_SCR1: return i_scratch[1];
            SPR_SCR2: return i_scratch[2];
            SPR_SCR3: return i_scratch[3];
            default:  return '0;
        endcase
    endfunction

    function automatic word_t operand(reg_ref_t ref_, word_t file_value, vaddr_t pc);
        if (!ref_.en) begin
            return '0;
        end else if (reads_pc(ref_)) begin
            return word_t'(pc);
        end
        return file_value;
    endfunction

    always_comb begin
        o_read_phys                = '0;
        o_read_phys[READ_PORT_Y_A] = phys_reg_of(op.src_a, i_sr.s);
        o_read_phys[READ_PORT_Y_B] = phys_reg_of(op.src_b, i_sr.s);
    end

    slot_t slot;

    always_comb begin
        slot       = '0;
        slot.valid = 1'b1;
        slot.op    = op;
        slot.a     = operand(op.src_a, i_read_value[READ_PORT_Y_A], op.pc);
        slot.b     = operand(op.src_b, i_read_value[READ_PORT_Y_B], op.pc);
        if (op.op_class == OPC_RDSPR && op.spr != SPR_USP) begin
            slot.a = spr_value(op.spr);
        end
        slot.flags = i_sr.flags;
    end

    always_comb begin
        pair_d = pair_q;
        if (i_kill) begin
            pair_d = '0;
        end else if (!i_hold) begin
            pair_d = '0;
            if (issue) begin
                pair_d.y     = slot;
                pair_d.older = SLOT_Y;
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pair_q <= '0;
        end else begin
            pair_q <= pair_d;
        end
    end

    always_comb begin
        o_taken    = '0;
        o_taken[0] = issue;
    end

    assign o_pair = pair_q;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_head;
    assign unused_head = ^{i_head[HEAD_WIDTH-1:1], i_head_valid[HEAD_WIDTH-1:1], i_read_value[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
