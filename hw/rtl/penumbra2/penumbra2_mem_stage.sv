// penumbra2_mem_stage — Penumbra/2 memory-access stage.
//
// Specified by the MEM section of doc/internals/penumbra2/pipeline-stages.md.
// It reads the EX/MEM register, performs the data-side work, and latches the
// MEM/WB register for the writeback stage under the back-pressure handshake.
//
// Two paths share the stage:
//   - Pass-through (ALU / branch / divmul / RDSPR / WRSPR): a single-cycle
//     register move — EX's result flows straight to WB, no stall.
//   - Data memory (LDx / STx): a 2-cycle access against the BRAM-backed data
//     memory. MEM checks alignment combinationally on entry; on a clean
//     access it drives the memory address and asserts STALL for one cycle
//     (per Decision 11, single-MEM-STALL). The next cycle the registered
//     read is valid: a load extracts/extends its sub-word, a store commits
//     its (lane-replicated) write, and the instruction advances to MEM/WB.
//     While the access runs MEM injects a bubble into MEM/WB — so WB does not
//     re-commit the slot it just took — and back-pressures EX to hold the
//     access operands stable.
//
// Address note: until the D-side MMU lands, the effective address is used as
// the physical address directly — the same simplification the gen2 front-end
// makes on the I-side today. The real BRAM-backed L1 D-cache (with MMU
// translation and miss handling) replaces the flat data-memory stand-in
// later, behind this same dmem interface — the mirror of how the flat i-mem
// gives way to the I-cache behind the IF interface.
//
// Deferred (still guarded out): RDSYS's sysreg sideband (o_sys_dev/o_sys_reg
// drive + its own 1-cycle STALL). An assertion catches an RDSYS reaching the
// stage rather than letting it forward a stale ALU result as sysreg data.
//
// Handshake: MEM back-pressures EX on (a) the first cycle of a memory access
// and (b) whenever WB back-pressures it (i_stall_in). i_bubble (the
// fault-commit flush from WB) forces the MEM/WB slot to a bubble, wins over
// everything, and cancels an in-flight access (no store commit).
//
// The writeback value reaching WB is a single datum (o_wb_value): for a load
// it is the extracted memory data, otherwise EX's result (the doc's
// gpr_value and spr_value collapse here because no instruction both writes a
// GPR and writes an SPR — WB routes the one value by the mutually exclusive
// gpr_we / spr_we bits). o_wb_value_aux carries a dual write's second value.

module penumbra2_mem_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── EX/MEM input: the instruction leaving EX ─────────────────
    input  logic [OPC_W-1:0]      i_op_class,        // for the deferred-path guard (RDSYS)
    input  logic [MEM_OP_W-1:0]   i_mem_op,          // MEM_NONE / MEM_LOAD / MEM_STORE
    input  logic [1:0]            i_mem_size,        // MEM_SZ_BYTE / MEM_SZ_HALF / MEM_SZ_WORD
    input  logic                  i_sign_ext,        // load: 1=sign-extend, 0=zero-extend
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [3:0]            i_spr_sel,
    input  logic [31:0]           i_result,          // ALU result / link / divmul lo / load-store EA
    input  logic [31:0]           i_result_aux,      // a dual write's second (aux) value; don't-care otherwise
    input  logic [31:0]           i_store_data,      // STx data (pre lane-replication); don't-care otherwise
    input  logic [3:0]            i_flag_value,      // NZCV, packed as SR[3:0]
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_en,
    input  logic [31:0]           i_pc,
    input  logic                  i_valid,           // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,

    // ── Data memory (BRAM-backed; flat stand-in for the L1 D-cache) ──
    // Registered-read contract (see unified_mem.sv): the address is sampled at
    // the clock edge and the addressed word appears combinationally during
    // the next cycle. o_dmem_en is the read clock-enable (holds the output
    // when low, keeping it aligned with a stalled MEM).
    output logic [31:0]           o_dmem_addr,
    output logic [31:0]           o_dmem_wdata,
    output logic [3:0]            o_dmem_byte_en,
    output logic                  o_dmem_we,
    output logic                  o_dmem_en,
    input  logic [31:0]           i_dmem_rdata,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,        // WB cannot accept this cycle
    input  logic                  i_bubble,          // force this insn to a bubble (fault flush from WB)
    output logic                  o_stall,           // back-pressure to EX

    // ── MEM/WB register (to WB) ──────────────────────────────────
    output logic [OPC_W-1:0]      o_op_class,        // carried to the retire point (halt / trap dispatch)
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic [3:0]            o_spr_sel,
    output logic [31:0]           o_wb_value,        // GPR-or-SPR writeback datum (load data, or EX result)
    output logic [31:0]           o_wb_value_aux,    // a dual write's second (aux) value; don't-care otherwise
    output logic [3:0]            o_flag_value,
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_aux,
    output logic                  o_phys_dst_aux_en,
    output logic [31:0]           o_pc,
    output logic                  o_valid,
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec
);

    // Control nets assigned in the back-pressure always_comb below, declared
    // up here because the data-memory drive (a continuous assign) reads them.
    logic advance, next_valid;
    logic acc_phase, acc_phase_next;

    // ── Access classification + alignment check ──────────────────
    logic is_load, is_store, is_mem;
    assign is_load  = (i_mem_op == MEM_LOAD);
    assign is_store = (i_mem_op == MEM_STORE);
    assign is_mem   = i_valid & (is_load | is_store) & ~i_fault_pending;

    // Alignment: byte always OK; half needs EA[0]==0; word needs EA[1:0]==00.
    logic misaligned;
    always_comb begin
        case (i_mem_size)
            MEM_SZ_WORD: misaligned = (i_result[1:0] != 2'b00);
            MEM_SZ_HALF: misaligned = i_result[0];
            default:     misaligned = 1'b0;                  // byte (and reserved): aligned
        endcase
    end

    logic align_fault;
    assign align_fault = is_mem & misaligned;

    // A real (aligned, un-flushed) memory access drives the data memory.
    logic do_access;
    assign do_access = is_mem & ~misaligned & ~i_bubble;

    // ── 2-cycle access phase ─────────────────────────────────────
    // acc_phase 0 = launch cycle (drive address/read, assert STALL);
    //           1 = data-ready cycle (extract load / commit store, advance).
    logic mem_first;
    assign mem_first = do_access & ~acc_phase;

    // ── Store path: lane-replication + byte-enable ───────────────
    logic [31:0] store_wdata;
    byte_rep u_byte_rep (
        .i_data (i_store_data),
        .i_size (i_mem_size),
        .o_data (store_wdata)
    );

    // byte_en selects which of the four byte lanes a store writes, from the
    // access size and the low EA bits. Penumbra is little-endian: EA[1:0]=00
    // maps to bits [7:0] (lane 0), so lane index == byte_en bit == EA[1:0]
    // for a byte store. A word store enables all lanes; a halfword store
    // enables the low or high pair by EA[1]. The reserved size writes no
    // lane (the assertion below catches it reaching a real access).
    logic [3:0] byte_en;

    always_comb begin
        case (i_mem_size)
            MEM_SZ_WORD: byte_en = 4'b1111;
            MEM_SZ_HALF: byte_en = i_result[1] ? 4'b1100 : 4'b0011;
            MEM_SZ_BYTE: begin
                case (i_result[1:0])
                    2'b00: byte_en = 4'b0001;
                    2'b01: byte_en = 4'b0010;
                    2'b10: byte_en = 4'b0100;
                    2'b11: byte_en = 4'b1000;
                endcase
            end
            default:     byte_en = 4'b0000;   // reserved size: write nothing
        endcase
    end

    // ── Load path: sub-word extract / extend ─────────────────────
    logic [31:0] load_data;
    byte_ext u_byte_ext (
        .i_data     (i_dmem_rdata),
        .i_addr_lo  (i_result[1:0]),
        .i_size     (i_mem_size),
        .i_sign_ext (i_sign_ext),
        .o_data     (load_data)
    );

    // ── Data-memory drive ────────────────────────────────────────
    // Address is stable across both access cycles (EX is back-pressured), so
    // it can be driven unconditionally. A load launches its read on cycle 1;
    // a store commits its write on the advancing (completing) cycle so a
    // flush or WB back-pressure can still cancel/defer it.
    assign o_dmem_addr    = i_result;
    assign o_dmem_wdata   = store_wdata;
    assign o_dmem_byte_en = byte_en;
    assign o_dmem_en      = is_load  & mem_first;
    assign o_dmem_we      = is_store & do_access & acc_phase & advance;

    // ── Writeback-value select ───────────────────────────────────
    // A load delivers the extracted memory data; everything else forwards
    // EX's result (ALU value / link / divmul low half / SPR write datum).
    logic [31:0] wb_value;
    assign wb_value = is_load ? load_data : i_result;

    // ── Fault merge (alignment) ──────────────────────────────────
    // An incoming fault (from upstream) takes priority over a freshly
    // detected alignment fault on the same slot.
    logic       mem_fault_pending;
    logic [3:0] mem_fault_vec;
    assign mem_fault_pending = i_fault_pending | align_fault;
    assign mem_fault_vec     = i_fault_pending ? i_fault_vec : VEC_ALIGN;

    // ── Deferred-path guard ──────────────────────────────────────
    // RDSYS's sysreg sideband is not wired yet (no o_sys_dev/o_sys_reg drive,
    // no sysreg-read STALL), so an RDSYS reaching here would forward a stale
    // ALU result as if it were the sysreg datum. Loads/stores now flow.
    always_comb begin
        assert (!i_valid || i_op_class != OPC_RDSYS)
            else $error("penumbra2_mem_stage: RDSYS reached MEM (sysreg sideband not wired)");
    end

    // A real memory access never carries the reserved size encoding — decode
    // emits only the three defined sizes. If this fires, an illegal Format M
    // instruction slipped through to MEM with size 2'b11 (byte_en and the
    // extract path have no meaning for it).
    always_comb begin
        assert (!is_mem || i_mem_size != 2'b11)
            else $error("penumbra2_mem_stage: reserved memory access size reached MEM");
    end

    // ── Issue / back-pressure control ────────────────────────────
    // i_bubble (fault flush) wins over everything. A memory access's first
    // cycle stalls EX and bubbles MEM/WB; its second cycle advances like a
    // normal op. Downstream back-pressure (i_stall_in) holds MEM/WB intact.
    always_comb begin
        acc_phase_next = acc_phase;
        if (i_bubble) begin
            next_valid     = 1'b0;          // flush wins
            advance        = 1'b0;
            o_stall        = i_stall_in;
            acc_phase_next = 1'b0;          // cancel any in-flight access
        end else if (mem_first) begin
            next_valid     = 1'b0;          // bubble into MEM/WB while accessing
            advance        = 1'b0;
            o_stall        = 1'b1;          // hold the access operands in EX/MEM
            acc_phase_next = 1'b1;          // → data-ready cycle next
        end else if (i_stall_in) begin
            next_valid     = o_valid;       // hold MEM/WB unchanged
            advance        = 1'b0;
            o_stall        = 1'b1;
            // acc_phase held: a data-ready access waits for WB to accept it
        end else begin
            next_valid     = i_valid;       // advance: bubble in if i_valid=0
            advance        = i_valid;
            o_stall        = 1'b0;
            acc_phase_next = 1'b0;          // access (if any) completes here
        end
    end

    // ── State + MEM/WB register ──────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid   <= 1'b0;
            acc_phase <= 1'b0;
        end else begin
            o_valid   <= next_valid;
            acc_phase <= acc_phase_next;
            if (advance) begin
                o_op_class        <= i_op_class;
                o_gpr_we          <= i_gpr_we;
                o_spr_we          <= i_spr_we;
                o_flag_we         <= i_flag_we;
                o_spr_sel         <= i_spr_sel;
                o_wb_value        <= wb_value;
                o_wb_value_aux    <= i_result_aux;
                o_flag_value      <= i_flag_value;
                o_phys_dst        <= i_phys_dst;
                o_phys_dst_aux    <= i_phys_dst_aux;
                o_phys_dst_aux_en <= i_phys_dst_aux_en;
                o_pc              <= i_pc;
                o_fault_pending   <= mem_fault_pending;
                o_fault_vec       <= mem_fault_vec;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // MEM-owned handshake / access invariants the structure does not enforce.
    // (The deferred-path guard belongs to the always_comb assert above.)
    // ══════════════════════════════════════════════════════════

    // Advance precondition: the MEM/WB register latches a real instruction
    // only on a clean accept — a valid input, not flushed, not back-pressured,
    // and not the first (address-launch) cycle of a memory access.
    always_comb begin
        assert (!advance || (i_valid && !i_bubble && !i_stall_in && !mem_first))
            else $error("penumbra2_mem_stage: advance without a clean precondition");
    end

    // The first cycle of a memory access always back-pressures EX (the BRAM
    // read/write is still settling), and always moves the phase forward.
    assert property (@(posedge i_clk) disable iff (i_rst)
        mem_first |-> o_stall)
        else $error("penumbra2_mem_stage: first access cycle did not stall EX");
    assert property (@(posedge i_clk) disable iff (i_rst)
        mem_first |=> acc_phase)
        else $error("penumbra2_mem_stage: access phase did not advance after launch");

    // A store commits its write only on the cycle it advances out of MEM —
    // never on the launch cycle, under back-pressure, or on a flushed slot.
    always_comb begin
        assert (!o_dmem_we || (advance && acc_phase && !i_bubble))
            else $error("penumbra2_mem_stage: store write outside the commit cycle");
    end

    // A fault-commit flush from WB always lands as a bubble in MEM/WB: a
    // wrong-path or faulting instruction must never slip through to WB.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_valid)
        else $error("penumbra2_mem_stage: i_bubble did not flush the MEM/WB slot");

    // Back-pressure holds the MEM/WB slot intact: a stalled MEM (for
    // downstream, not its own access launch) neither drops nor fabricates its
    // valid bit (the lost- / duplicated-insn bug at a stall boundary).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && !mem_first) |=> $stable(o_valid))
        else $error("penumbra2_mem_stage: back-pressure changed o_valid");

    // ...and it does not swap the held instruction's identity under it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && !mem_first && o_valid) |=> $stable(o_phys_dst))
        else $error("penumbra2_mem_stage: back-pressure swapped the held MEM/WB slot");

endmodule
