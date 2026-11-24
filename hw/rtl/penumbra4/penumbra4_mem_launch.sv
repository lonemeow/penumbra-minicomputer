// penumbra4_mem_launch — MEM1: launches the access's translation and
// lookup.
//
// An access launches its translation and its L1-D lookup as its pair
// leaves, so a held pair's verdicts stay those of its own launch. MEM1
// gives the access its byte lanes and places a store's data in them. A
// misaligned access faults here, ahead of its translation
// (doc/system/architecture.md): a halfword at an odd address, a word at one
// not a multiple of four.
//
// RDSYS and WRSYS are not launched: one that reaches MEM1 stops the
// simulation.
//
// The output register is the pair MEM2 resolves.

module penumbra4_mem_launch
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic   i_clk,
    input  logic   i_rst,

    input  pair_t  i_pair,
    input  logic   i_kill,
    input  logic   i_hold,
    output logic   o_hold,

    // ── The access's translation and lookup ──────────────────────
    output logic   o_launch,
    output vaddr_t o_launch_vaddr,

    output pair_t  o_pair
);

    // offset is the address's byte within its word, addr[1:0].
    function automatic logic [3:0] byte_en_of(logic [1:0] offset, mem_size_t size);
        case (size)
            MEM_SZ_BYTE: return 4'b0001 << offset;
            MEM_SZ_HALF: begin
                if (offset[1]) begin
                    return 4'b1100;
                end
                return 4'b0011;
            end
            default:     return 4'b1111;
        endcase
    endfunction

    function automatic logic misaligned(logic [1:0] offset, mem_size_t size);
        case (size)
            MEM_SZ_HALF: return offset[0];
            MEM_SZ_WORD: return offset != 2'b00;
            default:     return 1'b0;
        endcase
    endfunction

    slot_t y;
    slot_t y_out;
    logic  is_access;
    logic  leaving;
    word_t lanes;

    assign y         = i_pair.y;
    assign is_access = y.valid && (y.op.op_class == OPC_MEM);
    assign leaving   = y.valid && !i_hold && !i_kill;

    byte_rep u_byte_rep (
        .i_data (y.b),
        .i_size (y.op.size),
        .o_data (lanes)
    );

    always_comb begin
        y_out = y;
        if (is_access) begin
            y_out.byte_en = byte_en_of(y.addr[1:0], y.op.size);
            if (y.op.store) begin
                y_out.wdata = lanes;
            end
            if (misaligned(y.addr[1:0], y.op.size) && !y.op.fault.valid) begin
                y_out.op.fault.valid    = 1'b1;
                y_out.op.fault.vector   = VEC_ALIGN;
                y_out.op.fault.mmu_type = FAULT_ALIGN;
                if (y.op.store) begin
                    y_out.op.fault.access = ACC_WRITE;
                end else begin
                    y_out.op.fault.access = ACC_READ;
                end
            end
        end
    end

    assign o_launch       = leaving && is_access;
    assign o_launch_vaddr = y.addr;

    pair_t pair_q, pair_d;

    always_comb begin
        pair_d = pair_q;
        if (i_kill) begin
            pair_d = '0;
        end else if (!i_hold) begin
            pair_d   = i_pair;
            pair_d.y = y_out;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pair_q <= '0;
        end else begin
            pair_q <= pair_d;
        end
    end

    assign o_hold = i_hold;
    assign o_pair = pair_q;

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && i_pair.y.valid) begin
            assert (!(i_pair.y.op.op_class inside {OPC_RDSYS, OPC_WRSYS}))
                else $fatal(1, "penumbra4_mem_launch: a sysreg op at 0x%08x, which MEM1 does not launch",
                            i_pair.y.op.pc);
        end
    end

endmodule
