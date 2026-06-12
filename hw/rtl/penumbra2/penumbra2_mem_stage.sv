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
// MMU: the launch cycle also drives the MMU's D-side translate port (port B)
// with the effective address; the registered verdict (paddr, hit, fault)
// resolves on the data-ready cycle and holds until the next query — exactly
// where the VIPT L1's tag compare consumes it. A TLB miss or protection
// fault tags the slot inert (fault_pending), and the FAULT_ADDR/FAULT_STATUS
// commit payload (faulting vaddr + composed status) rides MEM/WB so WB can
// latch the MMU's architectural fault registers from its commit strobe.
//
// The flat data-memory stand-in is addressed by the *virtual* address at
// launch — the role the VIPT L1's vaddr index plays — but it has no paddr
// tag compare, so it returns vaddr-indexed data unconditionally. Until the
// real L1 D-cache replaces it behind this same dmem interface, a completing
// translated access must therefore be identity-mapped (asserted below);
// non-identity data mappings are the L1's tag compare to deliver.
//
// RDSYS shares the 2-cycle access FSM via the sysreg sideband: its launch
// cycle drives o_sys_dev/o_sys_reg + the read strobe o_sys_re, and the device's
// registered response arrives on i_sys_rdata the data-ready cycle (the same
// single-STALL timing as a D-cache hit). WRSYS does not reach MEM — it commits
// in EX as a drain-commit.
//
// Handshake: MEM back-pressures EX on (a) the first cycle of a memory or sysreg
// access and (b) whenever WB back-pressures it (i_stall_in). i_bubble (the
// fault-commit flush from WB) forces the MEM/WB slot to a bubble, wins over
// everything, and cancels an in-flight access (no store commit).
//
// The writeback value reaching WB is a single datum (o_wb_value): for a load
// it is the extracted memory data, for RDSYS the sysreg response, otherwise
// EX's result (the doc's
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
    input  logic [OPC_W-1:0]      i_op_class,        // selects the RDSYS sysreg-read path
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
    input  logic [31:0]           i_fault_status,    // carried payload; FAULT_NONE when none

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

    // ── MMU D-side translate (port B: query at launch, verdict at data-ready) ──
    // The query is driven on the access launch cycle alongside the data-memory
    // address; the registered verdict is valid on the data-ready cycle and
    // holds until the next query (mmu_bram's registered-read contract), so it
    // is stable on whichever cycle the access advances.
    output logic [31:0]           o_mmu_vaddr,
    output logic [2:0]            o_mmu_access_type, // ACC_READ / ACC_WRITE
    output logic                  o_mmu_req,         // query launch strobe
    input  logic                  i_user_mode,       // current privilege (query + align status)
    input  logic [31:0]           i_mmu_paddr,
    input  logic                  i_mmu_fault,       // any translation fault (miss / protection)
    input  logic [31:0]           i_mmu_fault_status, // composed by the MMU (Decision 16)

    // ── Sysreg sideband (RDSYS read; same registered-response timing) ──
    // RDSYS reads a CPU-internal sysreg device. MEM drives the device/register
    // selectors and the read strobe o_sys_re (the launch clock-enable); the
    // selected device's response is registered by the core and presented on
    // i_sys_rdata the next cycle — so RDSYS runs as a 2-cycle access exactly
    // like a D-cache hit. i_sys_dev/i_sys_reg come from the EX/MEM register.
    input  logic [3:0]            i_sys_dev,
    input  logic [3:0]            i_sys_reg,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic                  o_sys_re,
    input  logic [31:0]           i_sys_rdata,

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
    output logic [3:0]            o_fault_vec,
    // FAULT_ADDR/FAULT_STATUS commit payload — consumed by WB's commit strobe
    // into the MMU's architectural fault registers. The status is
    // self-qualifying: an address-carrying fault born here (alignment / TLB)
    // carries its composed type; anything else rides FAULT_NONE and leaves
    // the MMU registers untouched.
    output logic [31:0]           o_fault_vaddr,
    output logic [31:0]           o_fault_status
);

    // Control nets assigned in the back-pressure always_comb below, declared
    // up here because the data-memory drive (a continuous assign) reads them.
    logic advance, next_valid;
    logic acc_phase, acc_phase_next;

    // ── Access classification + alignment check ──────────────────
    logic is_load, is_store, is_mem, is_rdsys;
    assign is_load  = (i_mem_op == MEM_LOAD);
    assign is_store = (i_mem_op == MEM_STORE);
    assign is_mem   = i_valid & (is_load | is_store) & ~i_fault_pending;
    // RDSYS reads a sysreg via the sideband. It shares the 2-cycle access FSM
    // (launch drives the selectors + strobe, data-ready latches the registered
    // response) but touches no data memory and has no alignment concept.
    assign is_rdsys = i_valid & (i_op_class == OPC_RDSYS) & ~i_fault_pending;

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

    // A real, un-flushed access drives the 2-cycle FSM: an aligned memory
    // access (data memory) or an RDSYS (sysreg sideband).
    logic do_access;
    assign do_access = ((is_mem & ~misaligned) | is_rdsys) & ~i_bubble;

    // ── 2-cycle access phase ─────────────────────────────────────
    // acc_phase 0 = launch cycle (drive address/read, assert STALL);
    //           1 = data-ready cycle (extract load / commit store, advance).
    // Gated by ~i_stall_in: an access must not launch while WB back-pressures.
    // WB asserts i_stall_in only for a dual-write divmul holding MEM/WB across
    // its second (aux) write cycle; launching then would inject this access's
    // launch bubble over the held slot and drop the divmul's Rdh write. With
    // the launch deferred, the i_stall_in hold branch keeps the access waiting
    // in EX (acc_phase stays 0) until WB accepts, then it launches cleanly.
    logic mem_first;
    assign mem_first = do_access & ~acc_phase & ~i_stall_in;

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

    // ── MMU query drive (launch cycle, both loads and stores) ────
    // A misaligned access never launches (do_access excludes it), so the TLB
    // is queried only for accesses that can complete. RDSYS is not a memory
    // access and never translates.
    assign o_mmu_vaddr       = i_result;
    assign o_mmu_access_type = is_store ? ACC_WRITE : ACC_READ;
    assign o_mmu_req         = is_mem & mem_first;

    // ── MMU verdict consumption (data-ready cycle) ───────────────
    // i_mmu_fault covers every translation fault (miss and protection) with
    // its status composed by the MMU, and is 0 for a bypassing or idle port —
    // the only qualification left here is "this slot is a memory access at or
    // past its data-ready cycle" (a pass-through slot never ran a query).
    logic tlb_fault;
    assign tlb_fault = is_mem & acc_phase & i_mmu_fault;

    // ── Data-memory drive ────────────────────────────────────────
    // Address is stable across both access cycles (EX is back-pressured), so
    // it can be driven unconditionally. A load launches its read on cycle 1
    // unconditionally — the verdict is not known yet (the VIPT shape: data
    // read overlaps translation; a fault makes the slot inert so the garbage
    // data is never committed). A store commits its write on the advancing
    // (completing) cycle, gated on a clean translation, so a flush, WB
    // back-pressure, or a TLB fault cancels/defers it.
    assign o_dmem_addr    = i_result;
    assign o_dmem_wdata   = store_wdata;
    assign o_dmem_byte_en = byte_en;
    assign o_dmem_en      = is_load  & mem_first;
    assign o_dmem_we      = is_store & do_access & acc_phase & advance & ~tlb_fault;

    // ── Sysreg sideband drive ────────────────────────────────────
    // Mirrors the load read: o_sys_re is the launch clock-enable that tells the
    // core to register the selected device's response, valid on i_sys_rdata the
    // next (data-ready) cycle. The selectors come straight from the EX/MEM
    // register; they are stable across both cycles because EX is back-pressured.
    assign o_sys_dev = i_sys_dev;
    assign o_sys_reg = i_sys_reg;
    assign o_sys_re  = is_rdsys & mem_first;

    // ── Writeback-value select ───────────────────────────────────
    // A load delivers the extracted memory data, an RDSYS the registered sysreg
    // response; everything else forwards EX's result (ALU value / link / divmul
    // low half / SPR write datum).
    logic [31:0] wb_value;
    assign wb_value = is_load  ? load_data
                    : is_rdsys ? i_sys_rdata
                    :            i_result;

    // ── Fault merge + FAULT_ADDR/FAULT_STATUS composition ────────
    // Merges the slot's fault sources into the MEM/WB fault tag and the
    // FAULT_ADDR/FAULT_STATUS commit payload. Sources, in priority order
    // (exception-flow.md): an upstream fault riding the slot (the
    // instruction is already inert, no access launched, no address to
    // report — its status stays FAULT_NONE), then the two address faults
    // born here — alignment (detected at entry, access never launched) and
    // TLB (from the held port-B verdict at data-ready). Those two are
    // structurally exclusive: a misaligned access never queries the TLB.
    // An address fault's vector derives from its composed status — the
    // status type is the single classification (Decision 16). MEM composes
    // only the alignment status; the TLB status arrives composed.
    logic        mem_fault_pending;
    logic [3:0]  mem_fault_vec;
    logic [31:0] mem_fault_vaddr, mem_fault_status;

    always_comb begin
        mem_fault_pending = 1'b0;
        mem_fault_vec     = 4'b0;
        mem_fault_vaddr   = 32'b0;
        mem_fault_status  = 32'b0;        // FAULT_NONE

        if (i_fault_pending) begin
            mem_fault_pending = 1'b1;
            mem_fault_vec     = i_fault_vec;
            // An IF-side address fault's FAULT_ADDR is its own PC; the
            // composed status rode the pipe (FAULT_NONE for decode faults
            // and traps, which leave the MMU registers untouched).
            mem_fault_vaddr   = i_pc;
            mem_fault_status  = i_fault_status;
        end else if (align_fault | tlb_fault) begin
            mem_fault_pending = 1'b1;
            mem_fault_vaddr   = i_result;
            mem_fault_status  = align_fault
                ? {20'b0, i_user_mode, o_mmu_access_type, 4'b0, FAULT_ALIGN}
                : i_mmu_fault_status;
            mem_fault_vec     = fault_vec_of(mem_fault_status[3:0]);
        end
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
            // acc_phase held: an access waits for WB to accept it — either a
            // data-ready one (acc_phase 1) or one not yet launched (acc_phase
            // 0, mem_first suppressed by ~i_stall_in) deferring behind a
            // dual-write divmul that holds MEM/WB for its aux write.
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
                o_fault_vaddr     <= mem_fault_vaddr;
                o_fault_status    <= mem_fault_status;
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

    // The flat data-memory stand-in is vaddr-indexed with no paddr tag
    // compare (see header): a completing translated access must be
    // identity-mapped, or the returned/written data is for the wrong
    // physical location. The real VIPT L1's tag compare lifts this.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (is_mem && acc_phase && advance && !tlb_fault) |-> (i_mmu_paddr == i_result))
        else $error("penumbra2_mem_stage: non-identity D-mapping over the flat memory stand-in");

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
