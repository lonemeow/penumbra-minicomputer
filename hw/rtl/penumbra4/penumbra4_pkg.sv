// penumbra4_pkg -- Penumbra/4 (gen4) core-internal types.
//
// The payloads that cross gen4's stages (doc/internals/penumbra4/
// structure.md, "Payloads"): an op, the slot that carries it through a
// stage, and the pair of slots every stage register holds, with the
// encodings inside them. They describe gen4's microarchitecture, not the
// ISA, whose layouts and constants are penumbra_pkg's.
//
// Decode runs in IF2, so the fetch queue holds ops: an instruction enters
// it as an op of one, whether the fuser joins ops ahead of the queue or
// behind it.
/* verilator lint_off UNUSEDPARAM */
package penumbra4_pkg;
    import penumbra_pkg::*;

    // ── Register references ─────────────────────────────────────
    // R14 is two entries of the file, USP and SSP. R15 is the PC and no
    // entry at all, so SSP takes its number; a read of R15 takes the op's
    // own PC where its operand is captured. Every op in flight was issued
    // under the committed SR.S, since only a trap's entry and ERET change
    // it and both kill every op behind them, so a reference maps to its
    // entry by that one bit wherever it is read or written.
    typedef logic [3:0] rf_index_t;

    localparam rf_index_t RF_USP = REG_SP;
    localparam rf_index_t RF_SSP = REG_PC;

    typedef struct packed {
        logic       en;
        logic       user;     // R14 names USP in either mode: RDSPR USP, WRSPR USP
        reg_index_t index;
    } reg_ref_t;

    // en says whether an op uses the reference, not where it lands.
    /* verilator lint_off UNUSEDSIGNAL */
    function automatic rf_index_t rf_index_of(reg_ref_t ref_, logic supervisor);
        if (ref_.index == REG_SP && supervisor && !ref_.user) begin
            return RF_SSP;
        end
        return ref_.index;
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */

    // ── The ALU's function ──────────────────────────────────────
    // In Format R's op[3:0] order over its single-cycle region, so decode
    // takes an R-format ALU op's function straight from the instruction.
    // MOV, LLI and LLIS pass the second operand; LUI is an OR with the
    // immediate already at bit 16.
    typedef enum logic [3:0] {
        ALU_ADD,
        ALU_SUB,
        ALU_AND,
        ALU_OR,
        ALU_XOR,
        ALU_SHL,
        ALU_SHR,
        ALU_SAR,
        ALU_PASS,
        ALU_NOT,
        ALU_ADC,
        ALU_SBC
    } alu_fn_e;

    // ── An op's class ───────────────────────────────────────────
    // A class groups the ops issue pairs alike and the stages route
    // alike. What only one stage tells apart within a class, a load from
    // a store or a direct transfer from an indirect one, is a field of
    // the op.
    typedef enum logic [3:0] {
        OPC_ALU,
        OPC_MEM,
        OPC_BRANCH,
        OPC_FUNIT,
        OPC_RDSPR,
        OPC_WRSPR,
        OPC_RDSYS,
        OPC_WRSYS,
        OPC_IRQ_MASK,
        OPC_ERET,
        OPC_TRAP
    } op_class_e;

    // ── A prediction ────────────────────────────────────────────
    // What fetch did after an instruction: went on to the next word, or
    // to a predicted target. Execute's resolve checks it against where the
    // op goes.
    typedef struct packed {
        logic   taken;
        vaddr_t target;       // where fetch went, when taken
    } prediction_t;

    // ── A fault ─────────────────────────────────────────────────
    // A fault or trap an op takes at commit. The stage that finds one sets
    // it only while none is set: one an earlier stage found belongs to an
    // earlier part of the op, or keeps it from running at all.
    typedef struct packed {
        logic        valid;
        vector_t     vector;
        fault_type_t mmu_type;   // for the MMU's fault registers; FAULT_NONE leaves them
        access_t     access;     // the fetch, a load or a store
    } fault_t;

    // ── An op ───────────────────────────────────────────────────
    // What issue puts in a slot: one instruction, two the fuser joined, or
    // the op issue makes for an interrupt or a deferred bus fault. pc is
    // its first instruction's.
    typedef struct packed {
        op_class_e     op_class;
        alu_fn_e       alu_fn;
        reg_ref_t      src_a;
        reg_ref_t      src_b;        // a store's data among them
        logic          b_is_imm;     // the ALU takes imm as its second operand, not src_b
        word_t         imm;          // extended and placed as the op uses it
        reg_ref_t      dst;
        reg_ref_t      dst2;         // MUL's high half, DIV's remainder
        logic          reads_flags;
        logic          writes_flags;
        cond_t         cond;         // a branch's
        logic          indirect;     // JMP, JALR: the target is src_a's value, not target
        vaddr_t        target;       // a direct transfer's
        logic          store;        // an access's: a store, else a load
        mem_size_t     size;
        logic          sign_extend;
        logic [4:0]    funit_op;     // a peer unit's Format R op, which names the unit and its operation
        sysdev_t       sysdev;
        sysreg_index_t sysreg;
        spr_index_t    spr;
        logic          privileged;   // issue faults it in user mode
        vaddr_t        pc;
        prediction_t   prediction;
        fault_t        fault;
    } op_t;

    // ── A slot ──────────────────────────────────────────────────
    // An op in a stage and what the stages so far have computed for it.
    // Every stage register holds this one type, so a field means one thing
    // from issue to commit; a field no stage has reached yet is zero, and
    // synthesis drops the flops nothing reads.
    typedef struct packed {
        logic   valid;
        op_t    op;
        word_t  a;            // src_a's value, or the PC for R15
        word_t  b;            // src_b's value
        word_t  result;       // what dst takes: an ALU result, a link, a loaded word, a system read
        word_t  result2;      // what dst2 takes
        flags_t flags;
        vaddr_t addr;         // the access's address, from execute
    } slot_t;

    // ── A pair ──────────────────────────────────────────────────
    // What every stage register holds; a stage takes or holds both slots
    // at once. X and Y share the slot type, and the fields only Y's ops
    // use stay constant in X, where synthesis drops them.
    typedef enum logic {
        SLOT_X,
        SLOT_Y
    } slot_e;

    typedef struct packed {
        slot_t x;
        slot_t y;
        slot_e older;         // meaningful when both slots hold an op
    } pair_t;

endpackage
/* verilator lint_on UNUSEDPARAM */
