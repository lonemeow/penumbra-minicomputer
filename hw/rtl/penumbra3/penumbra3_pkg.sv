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
/* verilator lint_off UNUSEDPARAM */
package penumbra3_pkg;

  // ── Scoreboard physical entries ──────────────────────────────────
  // The scoreboard compares sources and destinations as *physical* entries.
  // Physical (not architectural) so R14's two banks (USP/SSP) and the
  // scoreboarded SPRs are distinct entries -- this is what catches the
  // WRSPR-USP / R14 aliasing hazard. Entry 0 (R0) is reserved-unused (the
  // decoder never names it); entries 1..13 map directly from R1..R13.
  //
  // No part of SR is a scoreboard entry: NZCV resolves by forwarding and the
  // S/I bits serialize via drain-commit, so neither is ever pending.
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

  // ── The core-to-uncore port's line ───────────────────────────────
  // The line a line read returns, in words: 16 bytes, the unit every cache
  // level stores (doc/internals/penumbra3/uncore-port.md).
  localparam int UNCORE_LINE_WORDS = 4;

  // ── The store buffer's depth ─────────────────────────────────────
  // Cacheable stores held between MEM2 and memory, a word each
  // (doc/internals/penumbra3/memory-performance.md, "Store buffer at the
  // core boundary").
  localparam int STORE_BUFFER_ENTRIES = 16;

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

  // ── Late writers ─────────────────────────────────────────────────
  // An op whose register result the scoreboard holds its readers back from:
  // a load's and a sysreg read's resolve in MEM2, after a reader issued
  // behind them would take its operands, and a divmul's two results are
  // written over two WB cycles. Every other result forwards to EX. A
  // divmul's second result is a late write too, of the aux destination.
  function automatic logic late_result(op_class_e op_class);
    return (op_class == OPC_LOAD) || (op_class == OPC_RDSYS)
        || (op_class == OPC_DIVMUL);
  endfunction

  // The late writers the spine reports to ID each cycle, a slot each: the
  // primary and aux destinations of EX, MEM1 and MEM2.
  localparam int LATE_WRITERS = 6;

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
  // unchanged, through ID -> EX -> MEM -> WB. The heavy word->bundle decode
  // runs once on the fetch-domain enqueue path, so ID's hazard cone starts
  // from these registered fields (P0.2), never from the raw instruction word.
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

  // ── Direction counters ───────────────────────────────────────────
  // The direction table holds saturating counters, and one in the upper half
  // predicts taken. Its width is set here alone: the table, the prediction
  // each slot carries and EX's training all hold a dir_counter_t, and read
  // or step one only through these functions.
  localparam int DIR_COUNTER_BITS = 2;
  typedef logic [DIR_COUNTER_BITS-1:0] dir_counter_t;

  function automatic logic dir_counter_taken(dir_counter_t counter);
    return counter[DIR_COUNTER_BITS-1];
  endfunction

  // One place toward the outcome, saturating at either end.
  function automatic dir_counter_t dir_counter_step(dir_counter_t counter, logic taken);
    if (taken) return (counter == '1) ? counter : counter + 1'b1;
    else       return (counter == '0) ? counter : counter - 1'b1;
  endfunction

  // ── Branch prediction carried with a slot ────────────────────────
  // What the front end predicted for a slot, carried from IF2's enqueue
  // through ID to EX, which verifies it (doc/internals/penumbra3/
  // branch-prediction.md, "Design"). taken means fetch already went to
  // target behind the slot; a slot predicted not taken has fetch running on
  // at its next PC, and its target is don't-care. Every slot is one or the
  // other: one the predictor did not recognise is predicted not taken, as
  // fetch ran on behind it. counter is the direction table's for the slot,
  // which EX steps toward a conditional branch's outcome to train the table.
  typedef struct packed {
    logic         taken;
    logic [31:0]  target;
    dir_counter_t counter;
  } prediction_t;

  // ── Datapath payload ─────────────────────────────────────────────
  // The resolved datapath state carried, as one packed value, across the
  // EX -> MEM -> WB pipeline registers alongside the ctrl_bundle_t. Copying it
  // as a unit (q <= d) is what keeps a register from silently dropping a field
  // the way a hand-rolled per-field copy can. `value` holds the stage's result
  // (EX: ALU / link / divmul-lo; MEM2: load data / sysreg read); the
  // destination routing, flags, PC, and fault verdict travel unchanged toward
  // the commit point. `store_data` is not here -- it is consumed in MEM2 and
  // never reaches WB, so it rides its own EX->MEM1 wire.
  typedef struct packed {
    logic [31:0]         value;          // primary result (ALU / link / load data / sysreg / ...)
    logic [31:0]         value_aux;      // second result of a dual-destination op
    logic [3:0]          flags;          // NZCV, packed as SR[3:0]
    logic [SB_IDX_W-1:0] phys_dst;
    logic                phys_dst_we;
    logic [SB_IDX_W-1:0] phys_dst_aux;
    logic                phys_dst_aux_we;
    logic [31:0]         pc;
    logic                fault_pending;
    logic [3:0]          fault_vec;
    logic [31:0]         fault_vaddr;    // faulting address: PC (EX-born) or EA (MEM-born)
    logic [31:0]         fault_status;
  } dpath_payload_t;

endpackage
/* verilator lint_on UNUSEDPARAM */
