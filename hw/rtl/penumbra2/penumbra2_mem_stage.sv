// penumbra2_mem_stage — Penumbra/2 memory-access stage.
//
// Specified by the MEM section of doc/internals/penumbra2/pipeline-stages.md.
// It reads the EX/MEM register, performs the data-side work, and latches the
// MEM/WB register for the writeback stage under the back-pressure handshake.
//
// Two paths share the stage:
//   - Pass-through (ALU / branch / divmul / RDSPR / WRSPR): a single-cycle
//     register move — EX's result flows straight to WB, no stall.
//   - Data memory (LDx / STx): a 2-cycle-minimum access against the D-side
//     front port (the BRAM-backed L1, or the flat stand-in at core level).
//     MEM checks alignment combinationally on entry; on a clean access it
//     launches the lookup (o_dmem_en samples the address — loads and stores
//     both, the cache's tag compare needs it) and asserts STALL (per
//     Decision 11, single-MEM-STALL). From the next cycle the request is
//     presented level (o_dmem_re for a load, o_dmem_we for a store) and the
//     access completes on the cycle i_dmem_busy is low — immediately for a
//     hit or the flat stand-in (busy tied 0), after the line fill or the
//     downstream round trip otherwise (drop-equals-valid: i_dmem_rdata is
//     valid exactly on the busy-drop cycle). A load then extracts/extends
//     its sub-word, a store's write has committed downstream, and the
//     instruction advances to MEM/WB. While the access runs MEM injects a
//     bubble into MEM/WB — so WB does not re-commit the slot it just took —
//     and back-pressures EX to hold the access operands stable.
//
// MMU: the launch cycle also drives the MMU's D-side translate port (port B)
// with the effective address; the registered verdict (paddr, hit, fault)
// resolves on the data-ready cycle and holds until the next query — exactly
// where the VIPT L1's tag compare consumes it. A TLB miss or protection
// fault tags the slot inert (fault_pending), and the FAULT_ADDR/FAULT_STATUS
// commit payload (faulting vaddr + composed status) rides MEM/WB so WB can
// latch the MMU's architectural fault registers from its commit strobe.
//
// RDSYS shares the 2-cycle access FSM via the sysreg sideband: its launch
// cycle drives o_sys_dev/o_sys_reg + the read strobe o_sys_re, and the device's
// registered response arrives on i_sys_rdata the data-ready cycle (the same
// single-STALL timing as a D-cache hit). WRSYS does not reach MEM — it commits
// in EX as a drain-commit.
//
// Handshake: MEM back-pressures EX on (a) every cycle of a memory or sysreg
// access before its completion (the launch cycle, plus every i_dmem_busy
// cycle after it) and (b) whenever WB back-pressures it (i_stall_in).
// i_bubble (the fault-commit flush from WB) forces the MEM/WB slot to a
// bubble and wins over everything. Neither i_bubble nor i_stall_in can land
// on a *launched* access: both are acquired the cycle their slot enters WB,
// which is the cycle this slot enters MEM — before any launch. A bubble
// therefore only ever suppresses a launch; it never has to cancel a
// transaction the memory side already accepted (the atomic-fill interface
// could not cancel one anyway). Both invariants are asserted below.
//
// The writeback value reaching WB is a single datum (o_wb_value): for a load
// it is the extracted memory data, for RDSYS the sysreg response, otherwise
// EX's result (the doc's
// gpr_value and spr_value collapse here because no instruction both writes a
// GPR and writes an SPR — WB routes the one value by the mutually exclusive
// gpr_we / spr_we bits). o_wb_value_aux carries a dual write's second value.

// keep_hierarchy: hold this stage boundary through synth_ecp5 so the stage
// places compactly instead of smearing across the die, and reads with real
// names in timing reports.
// Paired across the pipeline stages (if1 / if2 / spine / mem_stage).
(* keep_hierarchy = "yes" *)
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

    // ── Data memory (the D-side front port: BRAM L1 or flat stand-in) ──
    // Launch/resolve contract (cache_bram_vipt's front side; unified_mem is
    // the degenerate busy-never case): o_dmem_en samples the address at the
    // clock edge (the lookup launch — loads and stores both); from the next
    // cycle the request is presented level on o_dmem_re / o_dmem_we and held
    // stable until the completion cycle, which is the cycle i_dmem_busy is
    // low (drop-equals-valid: i_dmem_rdata carries the load data exactly
    // then). o_dmem_en doubles as the read clock-enable: low holds the
    // resolved output aligned with a stalled MEM.
    output logic [31:0]           o_dmem_addr,
    output logic [31:0]           o_dmem_wdata,
    output logic [3:0]            o_dmem_byte_en,
    output logic                  o_dmem_re,
    output logic                  o_dmem_we,
    output logic                  o_dmem_en,
    input  logic [31:0]           i_dmem_rdata,
    input  logic                  i_dmem_busy,
    input  logic                  i_dmem_fault,      // bus fault on the access (rides the busy-drop)

    // ── MMU D-side translate (port B: query at launch, verdict at data-ready) ──
    // The query is driven on the access launch cycle alongside the data-memory
    // address; the registered verdict is valid on the data-ready cycle and
    // holds until the next query (penumbra2_mmu's registered-read contract), so it
    // is stable on whichever cycle the access advances. The paddr leg of the
    // verdict goes to the D-cache's tag compare, not here — this stage
    // consumes only the fault verdict.
    output logic [31:0]           o_mmu_vaddr,
    output logic [2:0]            o_mmu_access_type, // ACC_READ / ACC_WRITE
    output logic                  o_mmu_req,         // query launch strobe
    input  logic                  i_user_mode,       // current privilege (query + align status)
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
    input  logic [BCAUSE_W-1:0]   i_bcause,          // cause carried by an incoming EX/MEM bubble
    output logic                  o_local_stall,     // back-pressure to EX (downstream-independent)
    output logic                  o_stall_load,      // stall cause: a load access holds the pipe (perfctr)
    output logic                  o_stall_store,     // stall cause: a store access holds the pipe (perfctr)

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
    output logic [BCAUSE_W-1:0]   o_bcause,          // stall cause carried by this slot when it is a bubble
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
    logic [BCAUSE_W-1:0] next_bcause;   // cause tag for the MEM/WB slot next edge
    logic acc_in_flight, acc_in_flight_next;

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

    // ── Access tracking ──────────────────────────────────────────
    // mem_first marks the launch cycle (drive address/read, assert STALL);
    // acc_in_flight then marks the access as launched on the data side, from
    // its data-ready cycle until the completion (busy-drop) cycle — one cycle
    // exactly when busy never rises, longer across a fill or round trip.
    // The launch is gated by ~i_stall_in: an access must not launch while WB
    // back-pressures. WB asserts i_stall_in only for a dual-write divmul
    // holding MEM/WB across its second (aux) write cycle; launching then
    // would inject this access's launch bubble over the held slot and drop
    // the divmul's Rdh write. With the launch deferred, the i_stall_in hold
    // branch keeps the access waiting in EX (not yet in flight) until WB
    // accepts, then it launches cleanly.
    // want_launch is the ungated launch *intent* (a clean access ready to
    // start a lookup); mem_first is the gated launch *action*, deferred while
    // WB back-pressures. They split so the stall need does not depend on the
    // downstream stall: MEM holds the pipe whenever it wants to launch, even
    // on the cycle the launch itself waits for WB.
    logic want_launch, mem_first;
    assign want_launch = do_access & ~acc_in_flight;
    assign mem_first   = want_launch & ~i_stall_in;

    // mem_local — MEM's downstream-independent back-pressure: it holds upstream
    // whenever it wants to launch (want_launch) or is waiting out a launched
    // access's data side (mem_busywait). The spine ORs this with the downstream
    // stalls; the pre-refactor o_stall was exactly mem_local | i_stall_in.
    logic mem_busywait;
    assign mem_busywait  = is_mem & acc_in_flight & i_dmem_busy;
    assign o_local_stall = want_launch | mem_busywait;

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
    assign tlb_fault = is_mem & acc_in_flight & i_mmu_fault;

    // ── Bus fault (late: reported at the access's completion) ────
    // The data side drops busy (the access completed) with i_dmem_fault set —
    // no device claimed the physical address, or a slave rejected it. Born at
    // the same point a clean completion would advance, and structurally
    // exclusive with both faults above: a TLB fault makes the slot inert (no
    // bus access launches) and a misaligned access never launches either.
    logic bus_fault;
    assign bus_fault = is_mem & acc_in_flight & ~i_dmem_busy & i_dmem_fault;

    // ── Data-memory drive ────────────────────────────────────────
    // Address is stable across all access cycles (EX is back-pressured), so
    // it can be driven unconditionally. The launch (o_dmem_en) fires for
    // loads and stores alike — the cache's tag lookup serves both — before
    // the verdict is known (the VIPT shape: the lookup overlaps translation;
    // a fault makes the slot inert so nothing is ever committed). From the
    // data-ready cycle the request is presented level: a load's o_dmem_re
    // holds until the busy-drop completion, a store's o_dmem_we likewise,
    // gated on a clean translation so a TLB fault keeps the write from ever
    // reaching the memory side (the cache's own i_fault gate is the same
    // verdict — both layers agree the slot is inert).
    assign o_dmem_addr    = i_result;
    assign o_dmem_wdata   = store_wdata;
    assign o_dmem_byte_en = byte_en;
    assign o_dmem_en      = is_mem   & mem_first;
    assign o_dmem_re      = is_load  & do_access & acc_in_flight;
    assign o_dmem_we      = is_store & do_access & acc_in_flight & ~tlb_fault;

    // ── Sysreg sideband drive ────────────────────────────────────
    // Mirrors the load read: o_sys_re is the launch clock-enable that tells the
    // core to register the selected device's response, valid on i_sys_rdata the
    // next (data-ready) cycle. The selectors come straight from the EX/MEM
    // register; they are stable across both cycles because EX is back-pressured.
    assign o_sys_dev = i_sys_dev;
    assign o_sys_reg = i_sys_reg;
    assign o_sys_re  = is_rdsys & mem_first;

    // ── Per-cause stall observability (perfctr) ──────────────────
    // A data access holds the pipeline this cycle — its launch cycle
    // (mem_first) or a busy-wait (acc_in_flight & i_dmem_busy) — split by
    // direction for the stall counters. RDSYS shares the access FSM but is not
    // a memory stall (is_mem gates it out), and a misaligned access never
    // launches (do_access, hence mem_first, excludes it), so neither pollutes
    // these. The completion cycle (busy drops, the slot advances) is productive
    // and not counted here.
    logic mem_data_stall;
    assign mem_data_stall = is_mem & (mem_first | (acc_in_flight & i_dmem_busy));
    assign o_stall_load   = mem_data_stall & is_load;
    assign o_stall_store  = mem_data_stall & is_store;

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
    // report — its status stays FAULT_NONE), then the three address faults
    // born here — alignment (detected at entry, access never launched), TLB
    // (from the held port-B verdict at data-ready), and bus (the access
    // completed reporting a no-device/slave fault). All three are
    // structurally exclusive: a misaligned access never queries the TLB, and
    // a bus fault implies a clean translation that launched a real access.
    // Each reports the faulting virtual address; the vector derives from the
    // composed status type — the single classification (Decision 16). The TLB
    // status arrives composed; MEM composes the alignment and bus statuses.
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
        end else if (align_fault | tlb_fault | bus_fault) begin
            // The three address faults born in MEM share FAULT_ADDR (the EA)
            // and differ only in their status word. They are mutually
            // exclusive: a misaligned access never launches a TLB query, and a
            // bus fault implies a clean translation that reached the bus.
            mem_fault_pending = 1'b1;
            mem_fault_vaddr   = i_result;
            // Status by source — alignment composes locally (MEM detects it),
            // the TLB status arrives composed from the MMU, the bus fault
            // composes here (the bus carries no status of its own):
            if      (align_fault) mem_fault_status = compose_fault_status(i_user_mode, o_mmu_access_type, FAULT_ALIGN);
            else if (tlb_fault)   mem_fault_status = i_mmu_fault_status;
            else if (bus_fault)   mem_fault_status = compose_fault_status(i_user_mode, o_mmu_access_type, FAULT_BUS);
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
    // i_bubble (fault flush) wins over everything — and can only coincide
    // with a launch it suppresses, never a launched access (asserted below).
    // A memory access's first cycle stalls EX and bubbles MEM/WB; it then
    // waits out i_dmem_busy and completes on the busy-drop cycle like a
    // normal op. Downstream back-pressure (i_stall_in) holds MEM/WB intact.
    // The MEM/WB bubble's cause for an access that holds the pipe. Data loads
    // and stores get their own precise bucket. An RDSYS shares this FSM but is
    // not data memory; it goes to the front-end residual (FLUSH), deliberately
    // not LOAD/STORE — reading the counters is itself an RDSYS, so charging it
    // to a precise back-end bucket would let the instrument perturb the bucket
    // it measures. Meaningful only on the bubble the access injects.
    logic [BCAUSE_W-1:0] access_bcause;
    assign access_bcause = is_store ? BCAUSE_STORE
                         : is_load  ? BCAUSE_LOAD
                         :            BCAUSE_FLUSH;   // RDSYS: residual, not a memory stall

    always_comb begin
        acc_in_flight_next = acc_in_flight;
        if (i_bubble) begin
            next_valid     = 1'b0;          // flush wins
            advance        = 1'b0;
            next_bcause    = BCAUSE_FLUSH;   // fault-commit flush from WB
        end else if (mem_first) begin
            next_valid     = 1'b0;          // bubble into MEM/WB while accessing
            advance        = 1'b0;
            acc_in_flight_next = 1'b1;       // launched: in flight from next cycle
            next_bcause    = access_bcause;
        end else if (is_mem && acc_in_flight && i_dmem_busy) begin
            // Busy-wait: the launched access's data side has not completed
            // (line fill / downstream round trip). Same posture as the launch
            // cycle — bubble into MEM/WB, hold the operands in EX — with the
            // request level held; acc_in_flight stays set (default above).
            next_valid     = 1'b0;
            advance        = 1'b0;
            next_bcause    = access_bcause;
        end else if (i_stall_in) begin
            next_valid     = o_valid;       // hold MEM/WB unchanged
            advance        = 1'b0;
            next_bcause    = o_bcause;       // hold the carried cause with the slot
            // An access not yet launched (mem_first suppressed by
            // ~i_stall_in) defers behind a dual-write divmul that holds
            // MEM/WB for its aux write.
        end else begin
            next_valid     = i_valid;       // advance: bubble in if i_valid=0
            advance        = i_valid;
            acc_in_flight_next = 1'b0;       // access (if any) completes here
            next_bcause    = i_bcause;       // propagate an incoming EX/MEM bubble's cause
        end
    end

    // ── State + MEM/WB register ──────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid   <= 1'b0;
            o_bcause  <= BCAUSE_FLUSH;       // cold pipe: the fill bubbles are front-end
            acc_in_flight <= 1'b0;
        end else begin
            o_valid   <= next_valid;
            o_bcause  <= next_bcause;        // travels with the slot, valid or bubble
            acc_in_flight <= acc_in_flight_next;
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
    // not the first (address-launch) cycle of a memory access, and not while
    // the data side is still busy with it.
    always_comb begin
        assert (!advance || (i_valid && !i_bubble && !i_stall_in && !mem_first
                             && !(is_mem && acc_in_flight && i_dmem_busy)))
            else $error("penumbra2_mem_stage: advance without a clean precondition");
    end

    // The first cycle of a memory access always back-pressures EX (the BRAM
    // read/write is still settling), and always moves the phase forward.
    assert property (@(posedge i_clk) disable iff (i_rst)
        mem_first |-> o_local_stall)
        else $error("penumbra2_mem_stage: first access cycle did not stall EX");
    assert property (@(posedge i_clk) disable iff (i_rst)
        mem_first |=> acc_in_flight)
        else $error("penumbra2_mem_stage: access phase did not advance after launch");

    // A store presents its write request only from its data-ready cycle on —
    // never on the launch cycle or on a flushed slot. (The request is a level
    // held until the busy-drop completion; the memory side commits it there.)
    always_comb begin
        assert (!o_dmem_we || (acc_in_flight && !i_bubble))
            else $error("penumbra2_mem_stage: store write outside the access window");
    end

    // A launch only ever fires into an idle data side — the front-port
    // contract (never launch a lookup while busy), upheld here because a
    // slot's predecessor completed its access before advancing out of MEM.
    always_comb begin
        assert (!(o_dmem_en && i_dmem_busy))
            else $error("penumbra2_mem_stage: lookup launched while the data side is busy");
    end

    // The WB-owned flush and back-pressure are acquired the cycle their slot
    // enters WB — the cycle this slot enters MEM, before any launch. Neither
    // can land on a launched access, so a transaction the memory side has
    // accepted always runs to completion (the atomic-fill interface could
    // not cancel one). If either fires, the in-order commit timing changed
    // and this stage's hold logic needs a real cancellation story.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_bubble && acc_in_flight))
        else $error("penumbra2_mem_stage: fault flush landed on a launched access");
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_stall_in && acc_in_flight))
        else $error("penumbra2_mem_stage: WB back-pressure landed on a launched access");

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
