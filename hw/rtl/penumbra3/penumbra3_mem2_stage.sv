// penumbra3_mem2_stage -- Penumbra/3 memory access resolve (second half).
//
// Combinational resolve logic plus penumbra3_load_complete and the MEM2/WB
// pipeline register. Reads the MEM1/MEM2 register and the verdicts the launch
// resolved a cycle later -- the translation (paddr, hit, faults) and the
// L1-D's (hit, word, victim way) -- and produces the writeback: sub-word
// extract for a load hit, the fault merge for the commit point, and the L1's
// store strobe. A load the L1 cannot serve (a miss, or an uncacheable page)
// is parked in penumbra3_load_complete as it leaves; that unit asks the
// uncore for it, installs the line, and writes the register back later.
//
// The back-end stall is the whole point: the only thing that holds the pipe is
// the registered o_load_pending out of the hold buffer, distributed by the
// spine -- never a combinational cache/TLB verdict reaching into issue.
//
// The spine holds a slot in MEM2 in two ways: behind a parked access
// (o_load_pending) and under WB back-pressure. Either way the slot's verdict
// stands every cycle it waits, and its effects -- the commit to WB, the park,
// the L1 write -- happen once, in the cycle it leaves (mem2_fire).

module penumbra3_mem2_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  int DL1_WAYS     = 2,     // the L1-D's ways
    localparam int DL1_WAY_BITS = $clog2(DL1_WAYS)
) (
    input  logic           i_clk,
    input  logic           i_rst,

    // -- MEM1/MEM2 input: the access entering MEM2 ----------------
    input  ctrl_bundle_t   i_bundle,
    input  dpath_payload_t i_payload,        // value = the effective address for a mem op
    input  logic [31:0]    i_store_wdata,
    input  logic [3:0]     i_byte_en,
    input  logic           i_align_fault,
    input  logic           i_valid,
    input  bcause_e        i_bcause,

    input  logic           i_user_mode,      // live mode, for fault-status composition

    // -- Translation verdict (spine drives from dtranslate) -------
    input  logic [31:0]    i_translate_paddr,
    input  logic           i_translate_cacheable,
    input  logic           i_translate_hit,
    input  logic           i_translate_miss_fault,
    input  logic           i_translate_prot_fault,

    // -- L1-D resolve, store strobe, counting ---------------------
    input  logic           i_dcache_hit,
    input  logic [31:0]    i_dcache_rdata,
    input  logic [DL1_WAY_BITS-1:0] i_dcache_victim_way,  // where a miss would fill
    input  logic           i_dcache_enabled,     // the L1's CTRL.ENABLE
    output logic           o_dcache_we,          // a store leaves MEM2: written on a hit
    output logic [31:0]    o_dcache_wdata,
    output logic [3:0]     o_dcache_byte_en,
    output logic           o_dcache_count_read,  // a cacheable load leaves MEM2
    output logic           o_dcache_count_write, // a cacheable store leaves MEM2

    // -- L1-D line install (the line runs from the uncore to the L1) --
    output logic           o_dcache_fill_we,
    output logic [DL1_WAY_BITS-1:0] o_dcache_fill_way,
    output logic [31:0]    o_dcache_fill_paddr,
    input  logic           i_dcache_extra_cycle, // the held lookup needs a cycle more

    // -- RDSYS sysreg response ------------------------------------
    input  logic [31:0]    i_sys_rdata,

    // -- Core-to-uncore port, data side ---------------------------
    // doc/internals/penumbra3/uncore-port.md. Reads only: a store's write
    // does not leave the core yet.
    output logic           o_dreq_valid,
    output logic           o_dreq_cacheable,
    output logic [31:0]    o_dreq_addr,
    output logic [3:0]     o_dreq_byte_en,
    input  logic           i_drsp_done,
    input  logic           i_rsp_fault,
    input  logic [UNCORE_LINE_WORDS-1:0][31:0] i_rsp_line,

    // -- Back-end hold + completion (to the spine) ----------------
    output logic                  o_load_pending,
    output logic                  o_complete,
    output logic [SB_IDX_W-1:0]   o_complete_dest,
    output logic [31:0]           o_complete_value,
    output logic                  o_complete_fault,

    // -- MEM2->EX forward source (EX's two-ahead leg) -------------
    output logic [31:0]           o_fwd_result,
    output logic [SB_IDX_W-1:0]   o_fwd_dst,
    output logic                  o_fwd_we,
    output logic                  o_fwd_valid,

    // -- Pipeline handshake ---------------------------------------
    input  logic           i_launch_hold,    // the MEM1/MEM2 register is frozen: the slot stays in MEM2
    input  logic           i_hold,           // freeze the MEM2/WB register (WB back-pressure)
    input  logic           i_bubble,         // fault-commit flush from WB

    // -- MEM2/WB register (to WB) ---------------------------------
    output ctrl_bundle_t   o_wb_bundle,
    output dpath_payload_t o_wb_payload,
    output logic           o_wb_valid,
    output bcause_e        o_wb_bcause
);

    // The slot in MEM2 takes effect in the cycle it leaves MEM2, and in no
    // other. i_launch_hold is the hold the MEM1/MEM2 register itself gets, so
    // the slot fires exactly when that register lets it go; a slot a fault
    // commit squashes never fires.
    logic mem2_fire;
    assign mem2_fire = i_valid & ~i_launch_hold & ~i_bubble;

    // -- Access classification ------------------------------------
    logic mem2_is_load, mem2_is_store, mem2_is_rdsys;
    assign mem2_is_load  = i_valid & (i_bundle.mem_op == MEM_LOAD);
    assign mem2_is_store = i_valid & (i_bundle.mem_op == MEM_STORE);
    assign mem2_is_rdsys = i_valid & (i_bundle.op_class == OPC_RDSYS);

    logic [2:0] mem2_acc_type;
    assign mem2_acc_type = mem2_is_store ? ACC_WRITE : ACC_READ;

    // -- Sub-word extract for a cache hit -------------------------
    logic [31:0] load_data;
    byte_ext u_byte_ext (
        .i_data     (i_dcache_rdata),
        .i_addr_lo  (i_payload.value[1:0]),
        .i_size     (i_bundle.mem_size),
        .i_sign_ext (i_bundle.sign_ext),
        .o_data     (load_data)
    );

    // -- Fault merge ----------------------------------------------
    // An upstream fault outranks the MEM-born faults (alignment, TLB miss,
    // protection), which are mutually exclusive. A bus fault on a missed load
    // rides the completion path, not here.
    logic align_f, tlbmiss_f, prot_f;
    assign align_f   = i_align_fault;
    assign tlbmiss_f = (mem2_is_load | mem2_is_store) & i_translate_miss_fault;
    assign prot_f    = i_translate_prot_fault;

    logic        mem2_fault;
    logic [3:0]  mem2_fault_vec;
    logic [31:0] mem2_fault_vaddr, mem2_fault_status;
    always_comb begin
        mem2_fault        = 1'b0;
        mem2_fault_vec    = 4'b0;
        mem2_fault_vaddr  = 32'b0;
        mem2_fault_status = 32'b0;               // FAULT_NONE
        if (i_payload.fault_pending) begin
            mem2_fault        = 1'b1;
            mem2_fault_vec    = i_payload.fault_vec;
            mem2_fault_vaddr  = i_payload.fault_vaddr;   // EX set it to the PC
            mem2_fault_status = i_payload.fault_status;
        end else if (align_f | tlbmiss_f | prot_f) begin
            mem2_fault       = 1'b1;
            mem2_fault_vaddr = i_payload.value;          // the EA
            if      (align_f)   mem2_fault_status = compose_fault_status(i_user_mode, mem2_acc_type, FAULT_ALIGN);
            else if (tlbmiss_f) mem2_fault_status = compose_fault_status(i_user_mode, mem2_acc_type, FAULT_TLB_MISS);
            else                mem2_fault_status = compose_fault_status(i_user_mode, mem2_acc_type, FAULT_PROT);
            mem2_fault_vec   = fault_vec_of(mem2_fault_status[3:0]);
        end
    end

    // -- Back-end load verdict -> completion unit -----------------
    // The L1's hit serves a load whatever its page's cacheability: a line is
    // resident for an uncacheable mapping only if software changed the page's
    // cacheability without invalidating (doc/system/mmu.md, "Uncached
    // Access"). A slot that fires either commits to WB or, a load the L1 did
    // not serve, parks; both key off the one load_miss, so they never
    // disagree.
    logic ld_valid, load_miss, mem2_commits;
    assign ld_valid     = mem2_is_load & ~mem2_fault;
    assign load_miss    = ld_valid & ~i_dcache_hit;
    assign mem2_commits = mem2_fire & ~load_miss;

    logic [31:0] lc_complete_data;
    logic [1:0]  lc_complete_size, lc_complete_addr_lo;
    logic        lc_complete_sign;

    penumbra3_load_complete #(
        .IDX_BITS   (SB_IDX_W),
        .WAYS       (DL1_WAYS),
        .LINE_WORDS (UNCORE_LINE_WORDS)
    ) u_load_complete (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_park             (mem2_fire & load_miss),
        .i_load_dest        (i_payload.phys_dst),
        .i_load_size        (i_bundle.mem_size),
        .i_load_sign        (i_bundle.sign_ext),
        .i_load_paddr       (i_translate_paddr),
        .i_load_cacheable   (i_translate_cacheable),
        .i_load_byte_en     (i_byte_en),
        .i_victim_way       (i_dcache_victim_way),
        .i_l1_enabled       (i_dcache_enabled),
        .i_mem2_slot_mem    (mem2_is_load | mem2_is_store),
        .o_req_valid        (o_dreq_valid),
        .o_req_cacheable    (o_dreq_cacheable),
        .o_req_addr         (o_dreq_addr),
        .o_req_byte_en      (o_dreq_byte_en),
        .i_rsp_done         (i_drsp_done),
        .i_rsp_fault        (i_rsp_fault),
        .i_rsp_line         (i_rsp_line),
        .o_fill_we          (o_dcache_fill_we),
        .o_fill_way         (o_dcache_fill_way),
        .o_fill_paddr       (o_dcache_fill_paddr),
        .i_l1_extra_cycle   (i_dcache_extra_cycle),
        .o_load_pending     (o_load_pending),
        .o_complete         (o_complete),
        .o_complete_dest    (o_complete_dest),
        .o_complete_data    (lc_complete_data),
        .o_complete_fault   (o_complete_fault),
        .o_complete_size    (lc_complete_size),
        .o_complete_addr_lo (lc_complete_addr_lo),
        .o_complete_sign    (lc_complete_sign)
    );

    // Completion writeback: extract the sub-word result from the parked
    // descriptor so a byte/halfword miss writes back correctly.
    byte_ext u_byte_ext_complete (
        .i_data     (lc_complete_data),
        .i_addr_lo  (lc_complete_addr_lo),
        .i_size     (lc_complete_size),
        .i_sign_ext (lc_complete_sign),
        .o_data     (o_complete_value)
    );

    // -- Writeback value + store write + forward source -----------
    logic [31:0] wb_value;
    assign wb_value = mem2_is_load  ? load_data
                    : mem2_is_rdsys ? i_sys_rdata
                    :                 i_payload.value;

    // A store writes the L1 as it leaves MEM2 fault-free, and each
    // cacheable access is counted in that same cycle. The L1 folds its own
    // enable and hit into both.
    assign o_dcache_wdata       = i_store_wdata;
    assign o_dcache_byte_en     = i_byte_en;
    assign o_dcache_we          = mem2_is_store & ~mem2_fault & mem2_fire;
    // The L1 counts each cacheable access that leaves MEM2 unfaulted.
    logic mem2_counted;
    assign mem2_counted         = ~mem2_fault & mem2_fire & i_translate_cacheable;
    assign o_dcache_count_read  = mem2_is_load  & mem2_counted;
    assign o_dcache_count_write = mem2_is_store & mem2_counted;

    // MEM2->EX forward (EX's two-ahead leg): a result that entered MEM2
    // already computed, so the leg starts from the MEM1/MEM2 register. A
    // load's or sysreg read's value resolves here, behind a block RAM read
    // and the tag compare; carried on into EX's operand mux and ALU, that
    // would make one cycle of the translate, the cache and the add, so
    // those forward from the MEM2/WB register instead.
    assign o_fwd_result = i_payload.value;
    assign o_fwd_dst    = i_payload.phys_dst;
    assign o_fwd_we     = i_payload.phys_dst_we;
    assign o_fwd_valid  = i_valid & ~mem2_is_load & ~mem2_is_rdsys & ~mem2_fault
                        & i_payload.phys_dst_we;

    // -- MEM2/WB register -----------------------------------------
    // Assemble the WB payload from the carried payload, overriding value (the
    // resolved datum) and the fault verdict; latch it as a unit.
    dpath_payload_t wb_payload_d;
    always_comb begin
        wb_payload_d               = i_payload;
        wb_payload_d.value         = wb_value;
        wb_payload_d.fault_pending = mem2_fault;
        wb_payload_d.fault_vec     = mem2_fault_vec;
        wb_payload_d.fault_vaddr   = mem2_fault_vaddr;
        wb_payload_d.fault_status  = mem2_fault_status;
    end

    // The bubble WB gets while an access is parked is that access's, whatever
    // waits behind it in MEM2.
    bcause_e mem2_bcause;
    assign mem2_bcause = (o_load_pending | load_miss) ? BCAUSE_LOAD
                       : mem2_is_store                ? BCAUSE_STORE
                       :                                i_bcause;

    // Only the valid bit carries the commit decision. The bundle and payload
    // load whenever MEM2 advances, and every consumer qualifies them with
    // o_wb_valid, so their ~350 enables stay off the verdict: the load-miss
    // decision behind them waits on the TLB permission check, cacheability
    // and the L1 hit. A parked access does not hold this register: it loads a
    // bubble every cycle the slot behind the access waits, so what is older
    // than the access drains to commit.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_wb_valid  <= 1'b0;
            o_wb_bcause <= BCAUSE_FLUSH;
        end else if (~i_hold) begin
            o_wb_valid  <= mem2_commits;
            o_wb_bcause <= i_bubble ? BCAUSE_FLUSH : mem2_bcause;
        end
        if (~i_hold) begin
            o_wb_bundle  <= i_bundle;
            o_wb_payload <= wb_payload_d;
        end
    end

    // -- Assertions (sim-only; Verilator --assert) ----------------
    // Both of the spine's holds on this slot reach it as i_launch_hold: a
    // parked access freezes the launch side, and WB back-pressure holds the
    // slot behind the register it freezes.
    always_comb
        assert (i_rst || !(o_load_pending && !i_launch_hold))
            else $error("penumbra3_mem2_stage: an access is parked and the launch side is not held");
    always_comb
        assert (i_rst || !(i_hold && !i_launch_hold))
            else $error("penumbra3_mem2_stage: the MEM2/WB register is held and the slot in MEM2 is not");

    // A held MEM1/MEM2 register keeps its slot through a flush, so a fault
    // commit must never find one held. Nothing commits while an access is
    // parked, and WB holds MEM2 only for a divmul's second write, which is
    // no fault commit.
    always_comb
        assert (i_rst || !(i_bubble && i_launch_hold))
            else $error("penumbra3_mem2_stage: a fault commit met a held MEM2 slot");

    // A protection fault implies a TLB hit (the verdict module guarantees it).
    always_comb
        assert (!prot_f || i_translate_hit)
            else $error("penumbra3_mem2_stage: protection fault without a TLB hit");

endmodule
