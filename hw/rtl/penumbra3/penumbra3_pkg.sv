// penumbra3_pkg -- Penumbra/3 (gen3) CPU-internal constants.
//
// Holds constants private to the gen3 core: the scoreboard physical-entry
// namespace, the control-bundle field encodings (op class, ALU function,
// memory op, bubble cause), and the packed control bundle itself. These
// describe the gen3 microarchitecture and are NOT part of the ISA -- the
// ISA / system contract (REG_*, COND_*, ACC_*, SPR_*, VEC_*, MEM_SZ_*)
// lives in penumbra_pkg (hw/rtl/common), which every gen3 module imports
// alongside this package. The two never collide: nothing here shadows a
// common symbol.
//
// gen3 is a full generation fork (its own core and its own on-chip memory
// hierarchy); see doc/internals/penumbra3/overview.md. It keeps its own
// copy of these microarch constants rather than sharing gen2's -- each
// generation reads as its own design, and gen2 stays byte-frozen.
/* verilator lint_off UNUSEDPARAM */
package penumbra3_pkg;

  // ── Scoreboard physical entries ──────────────────────────────────
  // The scoreboard is a flat array of *physical* entries, one pending bit
  // each. Physical (not architectural) so R14's two banks (USP/SSP) and the
  // scoreboarded SPRs are distinct entries -- this is what catches the
  // WRSPR-USP / R14 aliasing hazard. Entry 0 (R0) is reserved-unused (the
  // decoder never names it); entries 1..13 map directly from R1..R13.
  //
  // No part of SR is a scoreboard entry: NZCV resolves by forwarding and the
  // S/I bits serialize via drain-commit, so neither needs a pending bit.
  localparam int SB_NUM_ENTRIES = 22;                 // entries 0..21 (21 live)
  localparam int SB_IDX_W       = $clog2(SB_NUM_ENTRIES);

  localparam logic [SB_IDX_W-1:0] SB_USP  = 5'd14;  // R14 (user) / USP via RDSPR/WRSPR
  localparam logic [SB_IDX_W-1:0] SB_SSP  = 5'd15;  // R14 (supervisor)
  localparam logic [SB_IDX_W-1:0] SB_ESR  = 5'd16;  // RDSPR/WRSPR ESR
  localparam logic [SB_IDX_W-1:0] SB_EPC  = 5'd17;  // RDSPR/WRSPR EPC
  localparam logic [SB_IDX_W-1:0] SB_SCR0 = 5'd18;  // RDSPR/WRSPR SCR0
  localparam logic [SB_IDX_W-1:0] SB_SCR1 = 5'd19;  // RDSPR/WRSPR SCR1
  localparam logic [SB_IDX_W-1:0] SB_SCR2 = 5'd20;  // RDSPR/WRSPR SCR2
  localparam logic [SB_IDX_W-1:0] SB_SCR3 = 5'd21;  // RDSPR/WRSPR SCR3

  // ── ALU function ─────────────────────────────────────────────────
  // The EX-stage ALU select the decode hands forward. The enumerators take
  // values 0..11 in declaration order, which equals the ISA Format-R op[3:0]
  // in the single-cycle (op[4]=0) region -- so decode forwards op[3:0] as
  // alu_op with no remap. MOV is realized as PASS (operand mux routes the
  // moved value onto B).
  typedef enum logic [3:0] {
    ALU_ADD,    // 0
    ALU_SUB,    // 1
    ALU_AND,    // 2
    ALU_OR,     // 3
    ALU_XOR,    // 4
    ALU_SHL,    // 5
    ALU_SHR,    // 6
    ALU_SAR,    // 7
    ALU_PASS,   // 8  MOV: pass operand B
    ALU_NOT,    // 9
    ALU_ADC,    // 10 add with carry
    ALU_SBC     // 11 subtract with borrow
  } alu_op_e;

  // ── op_class: master instruction classification ──────────────────
  // The coarse class downstream stages branch on for *structural* routing
  // (EX pulses divmul.start on OPC_DIVMUL, resolves a branch on OPC_BRANCH,
  // ...) while the finer bundle fields carry per-instruction specifics. The
  // ALU-result forms (alu/imm/move) collapse into one OPC_ALU: the alu_op +
  // operand-mux fields already distinguish them, so a separate class buys
  // nothing.
  typedef enum logic [3:0] {
    OPC_ALU,        // 0  ALU/move -> GPR and/or flags
    OPC_LOAD,       // 1
    OPC_STORE,      // 2
    OPC_BRANCH,     // 3  Format B (incl. BL)
    OPC_JMP,        // 4  Format L JMP/JALR
    OPC_DIVMUL,     // 5
    OPC_RDSPR,      // 6
    OPC_WRSPR,      // 7
    OPC_RDSYS,      // 8
    OPC_WRSYS,      // 9
    OPC_ERET,       // 10
    OPC_EI,         // 11
    OPC_DI,         // 12
    OPC_SYSCALL,    // 13
    OPC_BREAK,      // 14
    OPC_ILLEGAL     // 15 reserved/undefined opcode
  } op_class_e;

  // ── Memory operation ─────────────────────────────────────────────
  typedef enum logic [1:0] {
    MEM_NONE,
    MEM_LOAD,
    MEM_STORE
  } mem_op_e;

  // ── Bubble cause (perfctr stall attribution) ─────────────────────
  // Each pipeline bubble carries the cause that injected it, set where the
  // bubble is born and propagated unchanged to the commit point, so a
  // non-retiring cycle is charged to its true cause. The commit point decodes
  // it into the SYSREG_CPU_STALL_* counters. Meaningful only on a bubble.
  typedef enum logic [2:0] {
    BCAUSE_NONE,    // a retiring slot -- charged to nothing
    BCAUSE_FLUSH,   // front-end redirect / fill, not memory-bound
    BCAUSE_IFETCH,  // front-end starved on a memory-bound fetch
    BCAUSE_LOAD,    // back end holding for a load access
    BCAUSE_STORE,   // back end holding for a store access
    BCAUSE_FUNIT,   // EX waiting on the multi-cycle unit (divmul)
    BCAUSE_HAZARD   // ID issue blocked by a pipeline interlock
  } bcause_e;

  // ── Control bundle ───────────────────────────────────────────────
  // The pre-decoded control word produced at fetch-FIFO enqueue and carried,
  // unchanged, through ID -> EX -> MEM -> WB. This is the gen3 thesis made
  // concrete: the heavy word->bundle decode runs once on the fetch-domain
  // enqueue path, so ID's hazard cone starts from these registered fields
  // (P0.2), never from the raw instruction word.
  //
  // The bundle carries *architectural* register selectors (Rd/Rs numbers +
  // is_spr + enables), not physical scoreboard indices: regmap stays in ID
  // because R14 banking depends on live supervisor mode, so the arch->phys
  // map is deliberately downstream of this struct.
  //
  // The fetch buffer stores it as an opaque $bits(ctrl_bundle_t)-wide word,
  // so the bundle width lives here, in the layout -- never as a hand-counted
  // PAYLOAD_W constant at the FIFO.
  typedef struct packed {
    // Master classification
    op_class_e   op_class;

    // Register references (feed penumbra3_regmap in ID). A source the
    // instruction does not read keeps its enable low (never a scoreboard
    // stall); is_pc marks an R15/PC source ID substitutes with the live PC.
    logic [3:0]  src_a_sel;
    logic        src_a_is_spr;
    logic        src_a_en;
    logic        src_a_is_pc;
    logic [3:0]  src_b_sel;
    logic        src_b_is_spr;
    logic        src_b_en;
    logic        src_b_is_pc;
    logic [3:0]  dst_sel;
    logic        dst_is_spr;     // destination is an SPR (else a GPR)
    logic        dst_we;         // writes its destination; CMP/TEST/stores/branches clear it
    logic [3:0]  dst_aux_sel;    // second (aux) write-only GPR dest (divmul hi half)
    logic        dst_aux_we;     // aux GPR write active (divmul hi half)

    // EX datapath controls
    alu_op_e     alu_op;
    logic [1:0]  divmul_op;      // = ISA op[1:0] (bit1 div/mul, bit0 unsigned)
    logic        a_from_pc;      // ALU operand A: 1=PC, 0=regfile src A
    logic        b_from_imm;     // ALU operand B: 1=immediate, 0=regfile src B
    logic [31:0] imm;
    logic [3:0]  cond;           // branch condition (Format B)

    // Flag interaction. NZCV travels by forwarding, not the scoreboard, so
    // these are plain control bits, never register references.
    logic        flags_updater;  // writes NZCV (EX computes; doubles as the WB flag write-enable)
    logic        flags_reader;   // reads NZCV via the flag-forward path (ADC/SBC, conditional branch)

    // Commit ordering. Serializing ops cannot overlap the rest of the pipe.
    logic        drain_commit;   // drain older instructions before this commits (ERET/WRSYS/EI/DI)
    logic        commit_wait;    // hold one extra cycle for the device latch (WRSYS)

    // MEM controls
    mem_op_e     mem_op;
    logic [1:0]  mem_size;       // MEM_SZ_BYTE / MEM_SZ_HALF / MEM_SZ_WORD
    logic        sign_ext;       // sub-word load sign-extend

    // Sysreg / SPR selects
    logic [3:0]  sys_dev;
    logic [3:0]  sys_reg;
    logic [3:0]  spr_sel;

    // Decode-time exceptions. Mode-independent: priv_op marks a supervisor-
    // only op, but ID raises the actual privilege fault against live mode and
    // composes the fault vector -- neither the fault nor the vector is a
    // stored bundle field.
    logic        is_trap;        // SYSCALL/BREAK
    logic        illegal;        // undefined opcode/operand form
    logic        priv_op;        // supervisor-only op (ID checks live mode)
  } ctrl_bundle_t;

endpackage
/* verilator lint_on UNUSEDPARAM */
