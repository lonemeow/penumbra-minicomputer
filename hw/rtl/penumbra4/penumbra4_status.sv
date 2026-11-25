// penumbra4_status — the status and save state: SR, EPC, ESR and the
// scratch registers.
//
// WB writes them, one op's effects at a time; every read is of what has
// committed. SR resets to supervisor mode with interrupts disabled. A trap's
// entry saves the PC and SR (doc/system/architecture.md, "Entry Sequence");
// ERET's SR is ESR's image, its reserved bits read as zero.

module penumbra4_status
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic          i_clk,
    input  logic          i_rst,

    input  status_write_t i_write,

    output sr_t           o_sr,
    output word_t         o_epc,
    output word_t         o_esr,
    output word_t [3:0]   o_scratch
);

    localparam sr_t SR_RESET = '{s: 1'b1, i: 1'b0, reserved: '0, flags: '0};

    sr_t         sr_q, sr_d;
    word_t       epc_q, epc_d;
    word_t       esr_q, esr_d;
    word_t [3:0] scratch_q, scratch_d;

    function automatic sr_t sr_of(word_t image);
        sr_t sr;
        sr          = sr_t'(image);
        sr.reserved = '0;
        return sr;
    endfunction

    always_comb begin
        sr_d      = sr_q;
        epc_d     = epc_q;
        esr_d     = esr_q;
        scratch_d = scratch_q;

        if (i_write.trap) begin
            epc_d = word_t'(i_write.epc);
            esr_d = word_t'(sr_q);
            sr_d.s = 1'b1;
            sr_d.i = 1'b0;
        end else if (i_write.eret) begin
            sr_d = sr_of(esr_q);
        end else begin
            if (i_write.flags_en) begin
                sr_d.flags = i_write.flags;
            end
            if (i_write.irq_mask_en) begin
                sr_d.i = i_write.irq_enable;
            end
            if (i_write.spr_en) begin
                case (i_write.spr)
                    SPR_EPC:  epc_d = i_write.spr_value;
                    SPR_ESR:  esr_d = i_write.spr_value;
                    SPR_SCR0: scratch_d[0] = i_write.spr_value;
                    SPR_SCR1: scratch_d[1] = i_write.spr_value;
                    SPR_SCR2: scratch_d[2] = i_write.spr_value;
                    SPR_SCR3: scratch_d[3] = i_write.spr_value;
                    default: begin
                    end
                endcase
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            sr_q      <= SR_RESET;
            epc_q     <= '0;
            esr_q     <= '0;
            scratch_q <= '0;
        end else begin
            sr_q      <= sr_d;
            epc_q     <= epc_d;
            esr_q     <= esr_d;
            scratch_q <= scratch_d;
        end
    end

    assign o_sr      = sr_q;
    assign o_epc     = epc_q;
    assign o_esr     = esr_q;
    assign o_scratch = scratch_q;

    // ── Contract ─────────────────────────────────────────────────
    // One op's effects at a time: a trap's entry and ERET never coincide,
    // and neither comes with the op-local writes a retiring op makes.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_write.trap || i_write.eret) |->
            !((i_write.trap && i_write.eret) || i_write.flags_en
              || i_write.irq_mask_en || i_write.spr_en))
        else $error("penumbra4_status: a trap's entry or ERET with other writes");

    // A trap's entry (doc/system/architecture.md, "Entry Sequence"): the PC
    // and SR saved, the SR in supervisor mode with interrupts disabled, its
    // flags kept.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_write.trap |=>
            epc_q == word_t'($past(i_write.epc)) && esr_q == word_t'($past(sr_q))
            && sr_q.s && !sr_q.i && sr_q.flags == $past(sr_q.flags))
        else $error("penumbra4_status: a trap's entry did not save the PC and SR or enter supervisor mode");

    // ERET restores the SR from ESR's image.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_write.eret |=> sr_q == sr_of($past(esr_q)))
        else $error("penumbra4_status: ERET did not restore the SR from ESR");

endmodule
