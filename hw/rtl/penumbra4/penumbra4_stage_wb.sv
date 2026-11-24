// penumbra4_stage_wb — WB: makes the pair's effects architectural, or
// takes its trap.
//
// WB decides from the pair MEM2 holds and registers every effect, so the
// register writes, the status writes, the redirect and the flush all leave
// flops and land a cycle later. A flush clears every stage, MEM2's
// register among them, so the pair that reached MEM2's register behind a
// trap or an ERET is never committed.
//
// A pair whose access the L1-D still carries waits here: WB decides
// nothing from it and holds it, and MEM2 puts the access's word or fault
// into it at the L1-D's done (doc/internals/penumbra4/structure.md, "A
// missed load holds its pair at WB").
//
// A fault takes its trap: the status saves the op's PC and SR, and fetch
// reads the vector. ERET takes ESR's image for the SR and redirects to EPC.
// A WRSYS resyncs: WB redirects to the instruction after it and flushes
// what was fetched before its write landed, since every later instruction
// must observe the write, its own fetch included (doc/system/sysregs.md).
// A BREAK's trap pulses program end (doc/internals/build-system.md, "The
// machine layer").

module penumbra4_stage_wb
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic              i_clk,
    input  logic              i_rst,

    input  pair_t             i_pair,
    input  logic              i_supervisor,   // SR.S as committed
    input  word_t             i_epc,
    output logic              o_hold,

    output pair_t             o_landing,      // the pair whose effects land this cycle
    output reg_write_t [1:0]  o_reg_write,
    output status_write_t     o_status_write,
    output wb_redirect_t      o_redirect,
    output logic              o_flush,
    output logic [1:0]        o_retire_count,
    output logic              o_prog_end
);

    typedef struct packed {
        pair_t             landing;
        reg_write_t [1:0]  reg_write;
        status_write_t     status_write;
        wb_redirect_t      redirect;
        logic              flush;
        logic [1:0]        retire_count;
        logic              prog_end;
    } effects_t;

    effects_t effects_q, effects_d;

    function automatic reg_write_t reg_write(reg_ref_t dst, word_t value, logic supervisor);
        reg_write_t write;
        write.en    = dst.en;
        write.phys = phys_reg_of(dst, supervisor);
        write.value = value;
        return write;
    endfunction

    // A slot carries more than WB reads of it.
    /* verilator lint_off UNUSEDSIGNAL */
    slot_t y;
    /* verilator lint_on UNUSEDSIGNAL */

    assign y = i_pair.y;

    always_comb begin
        effects_d = '0;

        if (y.valid && !y.access_pending && !effects_q.flush) begin
            effects_d.landing = i_pair;

            if (y.op.fault.valid) begin
                effects_d.status_write.trap = 1'b1;
                effects_d.status_write.epc  = y.op.pc;
                effects_d.redirect.valid     = 1'b1;
                effects_d.redirect.to_vector = 1'b1;
                effects_d.redirect.vector    = y.op.fault.vector;
                effects_d.flush              = 1'b1;
                effects_d.prog_end           = (y.op.fault.vector == VEC_BREAK);
            end else begin
                effects_d.retire_count = 2'd1;

                case (y.op.op_class)
                    OPC_ALU, OPC_BRANCH, OPC_MEM, OPC_RDSPR, OPC_RDSYS: begin
                        effects_d.reg_write[0] = reg_write(y.op.dst, y.result, i_supervisor);
                    end
                    OPC_FUNIT: begin
                        effects_d.reg_write[0] = reg_write(y.op.dst, y.result, i_supervisor);
                        effects_d.reg_write[1] = reg_write(y.op.dst2, y.result2, i_supervisor);
                    end
                    OPC_WRSPR: begin
                        if (y.op.spr == SPR_USP) begin
                            effects_d.reg_write[0] = reg_write(y.op.dst, y.result, i_supervisor);
                        end else begin
                            effects_d.status_write.spr_en    = 1'b1;
                            effects_d.status_write.spr       = y.op.spr;
                            effects_d.status_write.spr_value = y.result;
                        end
                    end
                    OPC_IRQ_MASK: begin
                        effects_d.status_write.irq_mask_en = 1'b1;
                        effects_d.status_write.irq_enable  = y.op.irq_enable;
                    end
                    OPC_ERET: begin
                        effects_d.status_write.eret = 1'b1;
                        effects_d.redirect.valid    = 1'b1;
                        effects_d.redirect.target   = vaddr_t'(i_epc);
                        effects_d.flush             = 1'b1;
                    end
                    OPC_WRSYS: begin
                        effects_d.redirect.valid  = 1'b1;
                        effects_d.redirect.target = y.op.pc + vaddr_t'(4);
                        effects_d.flush           = 1'b1;
                    end
                    default: begin
                    end
                endcase

                if (y.op.writes_flags && y.op.op_class != OPC_ERET) begin
                    effects_d.status_write.flags_en = 1'b1;
                    effects_d.status_write.flags    = y.flags;
                end
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            effects_q <= '0;
        end else begin
            effects_q <= effects_d;
        end
    end

    // A flush lands a cycle after WB decides it, and in that cycle the pair
    // behind could leave MEM2 and hand the L1-D an access nothing can
    // recall. Holding while deciding keeps it in MEM1's register for the
    // flush to clear (doc/internals/penumbra4/structure.md, "A trap holds
    // the pair behind it"). The decision reads only MEM2's register, so the
    // hold stays early.
    assign o_hold         = (y.valid && y.access_pending) || effects_d.flush;
    assign o_landing      = effects_q.landing;
    assign o_reg_write    = effects_q.reg_write;
    assign o_status_write = effects_q.status_write;
    assign o_redirect     = effects_q.redirect;
    assign o_flush        = effects_q.flush;
    assign o_retire_count = effects_q.retire_count;
    assign o_prog_end     = effects_q.prog_end;

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && i_pair.x.valid) begin
            assert (1'b0)
                else $fatal(1, "penumbra4_stage_wb: an op in slot X, which WB does not carry");
        end
    end

endmodule
