// penumbra3_mem2_stage -- Penumbra/3 memory access resolve (second half).
//
// Combinational resolve logic plus penumbra3_mem_completion and the MEM2/WB
// pipeline register. Reads the MEM1/MEM2 register and the verdicts the launch
// resolved a cycle later -- the translation (paddr, hit, faults) and the
// L1-D's (hit, word) -- and produces the writeback: sub-word extract for a
// load hit, the fault merge for the commit point, and the L1's store strobe.
// A load the L1 had no word for and every store leave as an access the L1-D
// takes (doc/internals/penumbra3/l1-cache.md), and their descriptors park in
// penumbra3_mem_completion, which writes a load's register back when the
// L1-D's access is done.
//
// The back-end stall is the whole point: the only thing that holds the pipe is
// the L1-D's registered busy, reported as o_mem_pending and distributed by the
// spine -- never a combinational cache/TLB verdict reaching into issue.
//
// The spine holds a slot in MEM2 in two ways: behind a parked access
// (o_mem_pending) and under WB back-pressure. Either way the slot's verdict
// stands every cycle it waits, and its effects -- the move into WB, the park,
// the L1 write -- happen once, in the cycle it leaves (mem2_fire).

module penumbra3_mem2_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
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
    // The data and lanes serve a store's hit write and its access both.
    input  logic           i_dcache_hit,
    input  logic [31:0]    i_dcache_rdata,
    output logic           o_dcache_we,          // a store leaves MEM2: written on a hit
    output logic [31:0]    o_dcache_wdata,
    output logic [3:0]     o_dcache_byte_en,
    output logic           o_dcache_count_read,  // a cacheable load leaves MEM2
    output logic           o_dcache_count_write, // a cacheable store leaves MEM2

    // -- L1-D access: a load it had no word for, or a store's write --
    // doc/internals/penumbra3/l1-cache.md
    output logic           o_dcache_access_req,  // one cycle, as the access leaves MEM2
    output logic           o_dcache_access_we,
    output logic [31:0]    o_dcache_access_addr, // physical
    output logic           o_dcache_access_cacheable,
    input  logic           i_dcache_busy,
    input  logic           i_dcache_access_done,
    input  logic [31:0]    i_dcache_access_word,
    input  logic           i_dcache_access_fault,

    // -- RDSYS sysreg response ------------------------------------
    input  logic [31:0]    i_sys_rdata,

    // -- Back-end hold + completion (to the spine) ----------------
    output logic                  o_mem_pending,
    output logic                  o_complete,
    output logic                  o_complete_we,     // with o_complete: a load
    output logic                  o_complete_retires, // one cycle, with o_complete
    output logic [SB_IDX_W-1:0]   o_complete_dest,
    output logic [31:0]           o_complete_value,
    output logic                  o_complete_fault,  // one cycle, with o_complete
    output logic [31:0]           o_complete_pc,
    output logic [31:0]           o_complete_vaddr,
    output logic [31:0]           o_complete_status,

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

    // -- What a slot leaving MEM2 does -----------------------------
    // The L1's hit serves a load whatever its page's cacheability: a line is
    // resident for an uncacheable mapping only if software changed the page's
    // cacheability without invalidating (doc/system/mmu.md, "Uncached
    // Access").
    //
    // A fired slot (mem2_fire) is an instruction and, for a load or a store,
    // an access. Each leaves MEM2 its own way:
    //   mem2_retires  the instruction: the MEM2/WB register takes it and it
    //                 retires at WB, carrying its fault if it has one; its
    //                 retire needs nothing more from memory.
    //   mem2_parks    the access: the L1-D takes it, and the completion unit
    //                 parks its descriptor (i_park) and reports the
    //                 completion, fault and all, when the L1-D's access is
    //                 done.
    // An instruction that does not retire here finishes with its access's
    // completion.
    // An access with an MMU-born fault asks memory for nothing. The rule for
    // which completions may come after the instruction retires is
    // doc/internals/penumbra3/memory-completion.md, "Fault precision".
    logic mem2_parks, mem2_retires;
    always_comb begin
        mem2_retires = 1'b0;
        mem2_parks   = 1'b0;

        if (mem2_fire) begin
            if (mem2_fault) begin
                mem2_retires = 1'b1;
                mem2_parks   = 1'b0;
            end else if (mem2_is_load) begin
                if (i_dcache_hit) begin
                    mem2_retires = 1'b1;
                    mem2_parks   = 1'b0;
                end else begin
                    mem2_retires = 1'b0;
                    mem2_parks   = 1'b1;
                end
            end else if (mem2_is_store) begin
                if (i_translate_cacheable) begin
                    mem2_retires = 1'b1;
                    mem2_parks   = 1'b1;
                end else begin
                    mem2_retires = 1'b0;
                    mem2_parks   = 1'b1;
                end
            end else begin
                mem2_retires = 1'b1;
                mem2_parks   = 1'b0;
            end
        end
    end

    logic [31:0] mc_complete_data;
    logic [1:0]  mc_complete_size, mc_complete_addr_lo;
    logic        mc_complete_sign, mc_pending_store;

    // The access leaves for the L1-D as its descriptor parks.
    assign o_dcache_access_req       = mem2_parks;
    assign o_dcache_access_we        = mem2_is_store;
    assign o_dcache_access_addr      = i_translate_paddr;
    assign o_dcache_access_cacheable = i_translate_cacheable;
    assign o_mem_pending             = i_dcache_busy;

    penumbra3_mem_completion #(
        .IDX_BITS (SB_IDX_W)
    ) u_mem_completion (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_park             (mem2_parks),
        .i_access_we        (mem2_is_store),
        .i_access_pc        (i_payload.pc),
        .i_access_vaddr     (i_payload.value),
        .i_access_user      (i_user_mode),
        .i_access_cacheable (i_translate_cacheable),
        .i_load_dest        (i_payload.phys_dst),
        .i_load_size        (i_bundle.mem_size),
        .i_load_sign        (i_bundle.sign_ext),
        .i_l1_busy          (i_dcache_busy),
        .i_l1_done          (i_dcache_access_done),
        .i_l1_word          (i_dcache_access_word),
        .i_l1_fault         (i_dcache_access_fault),
        .o_pending_store    (mc_pending_store),
        .o_complete         (o_complete),
        .o_complete_we      (o_complete_we),
        .o_complete_retires (o_complete_retires),
        .o_complete_dest    (o_complete_dest),
        .o_complete_data    (mc_complete_data),
        .o_complete_size    (mc_complete_size),
        .o_complete_addr_lo (mc_complete_addr_lo),
        .o_complete_sign    (mc_complete_sign),
        .o_complete_fault   (o_complete_fault),
        .o_complete_pc      (o_complete_pc),
        .o_complete_vaddr   (o_complete_vaddr),
        .o_complete_status  (o_complete_status)
    );

    // Completion writeback: extract the sub-word result from the parked
    // descriptor so a byte/halfword miss writes back correctly.
    byte_ext u_byte_ext_complete (
        .i_data     (mc_complete_data),
        .i_addr_lo  (mc_complete_addr_lo),
        .i_size     (mc_complete_size),
        .i_sign_ext (mc_complete_sign),
        .o_data     (o_complete_value)
    );

    // -- Writeback value + store write + forward source -----------
    logic [31:0] wb_value;
    assign wb_value = mem2_is_load  ? load_data
                    : mem2_is_rdsys ? i_sys_rdata
                    :                 i_payload.value;

    // A store writes the L1 as it leaves MEM2 fault-free, and each
    // cacheable access is counted in that same cycle. The L1 folds its own
    // enable and hit into both. Its access takes the same data and lanes.
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
    logic load_miss;
    assign load_miss = mem2_is_load & ~mem2_fault & ~i_dcache_hit;

    bcause_e mem2_bcause;
    assign mem2_bcause = o_mem_pending ? (mc_pending_store ? BCAUSE_STORE : BCAUSE_LOAD)
                       : load_miss     ? BCAUSE_LOAD
                       : mem2_is_store ? BCAUSE_STORE
                       :                 i_bcause;

    // Only the valid bit carries the retire decision. The bundle and payload
    // load whenever MEM2 advances, and every consumer qualifies them with
    // o_wb_valid, so their ~350 enables stay off the verdict: the retire
    // decision behind them waits on the TLB permission check, cacheability
    // and the L1 hit. A parked access does not hold this register: it loads a
    // bubble every cycle the slot behind the access waits, so what is ahead
    // of that slot drains to retire.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_wb_valid  <= 1'b0;
            o_wb_bcause <= BCAUSE_FLUSH;
        end else if (~i_hold) begin
            o_wb_valid  <= mem2_retires;
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
        assert (i_rst || !(o_mem_pending && !i_launch_hold))
            else $error("penumbra3_mem2_stage: an access is parked and the launch side is not held");
    always_comb
        assert (i_rst || !(i_hold && !i_launch_hold))
            else $error("penumbra3_mem2_stage: the MEM2/WB register is held and the slot in MEM2 is not");

    // Every fired slot goes somewhere: one that neither retires nor parks
    // is an instruction lost from the pipe.
    always_comb
        assert (i_rst || !mem2_fire || mem2_retires || mem2_parks)
            else $error("penumbra3_mem2_stage: a fired slot neither retired nor parked");

    // A held MEM1/MEM2 register keeps its slot through a flush, so a fault
    // commit must never find one held. While an access is parked, WB sees
    // bubbles, or a store that retired as its write parked, carrying no
    // fault. A completion's bus fault finds o_mem_pending low: the faulted
    // access installed nothing, so no held lookup keeps the L1-D busy past
    // its done. And WB holds MEM2 only for a divmul's second write, which is
    // no fault commit.
    always_comb
        assert (i_rst || !(i_bubble && i_launch_hold))
            else $error("penumbra3_mem2_stage: a fault commit met a held MEM2 slot");

    // A protection fault implies a TLB hit (the verdict module guarantees it).
    always_comb
        assert (!prot_f || i_translate_hit)
            else $error("penumbra3_mem2_stage: protection fault without a TLB hit");

endmodule
