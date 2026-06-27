// penumbra3_mem2_stage -- Penumbra/3 memory access resolve (second half).
//
// Combinational resolve logic plus penumbra3_load_complete and the MEM2/WB
// pipeline register. Reads the MEM1/MEM2 register and the verdicts the launch
// resolved a cycle later -- the translation (paddr, hit, faults) and the
// D-cache tag compare (hit, data) -- and produces the writeback: sub-word
// extract for a load hit, the fault merge for the commit point, and the store
// write-through. A load that misses (cacheable miss or uncached) is handed to
// the hold buffer and evicted; its registered completion writes back later.
//
// The back-end stall is the whole point: the only thing that holds the pipe is
// the registered o_load_pending out of the hold buffer, distributed by the
// spine -- never a combinational cache/TLB verdict reaching into issue.

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

    // -- D-cache resolve + store write ----------------------------
    input  logic           i_dcache_hit,
    input  logic [31:0]    i_dcache_rdata,
    output logic [31:0]    o_dcache_paddr,
    output logic           o_dcache_we,
    output logic [31:0]    o_dcache_wdata,
    output logic [3:0]     o_dcache_byte_en,

    // -- RDSYS sysreg response ------------------------------------
    input  logic [31:0]    i_sys_rdata,

    // -- Bus-master line fill (load_complete <-> bus master) ------
    output logic           o_launch_fill,
    output logic [31:0]    o_fill_paddr,
    output logic           o_fill_cacheable,
    input  logic           i_fill_done,
    input  logic [31:0]    i_fill_data,
    input  logic           i_fill_fault,

    // -- Back-end hold + completion (to the spine) ----------------
    output logic                  o_load_pending,
    output logic                  o_complete,
    output logic [SB_IDX_W-1:0]   o_complete_dest,
    output logic [31:0]           o_complete_value,
    output logic                  o_complete_fault,

    // -- MEM2->EX forward source (EX's load-use / two-ahead leg) --
    output logic [31:0]           o_fwd_result,
    output logic [SB_IDX_W-1:0]   o_fwd_dst,
    output logic                  o_fwd_we,
    output logic                  o_fwd_valid,

    // -- Pipeline handshake ---------------------------------------
    input  logic           i_hold,           // freeze the MEM2/WB register (pipe stall)
    input  logic           i_bubble,          // fault-commit flush from WB

    // -- MEM2/WB register (to WB) ---------------------------------
    output ctrl_bundle_t   o_wb_bundle,
    output dpath_payload_t o_wb_payload,
    output logic           o_wb_valid,
    output bcause_e        o_wb_bcause
);

    logic mem2_fire;                            // the MEM2 slot retires to WB cleanly
    assign mem2_fire = ~i_hold & ~i_bubble;

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

    // -- Back-end load verdict -> hold buffer ---------------------
    // An uncached load never "hits" the cache, so it takes the fill path too.
    // load_miss keys off the same effective hit fed to load_complete, so the
    // two never disagree on whether the load is held.
    logic ld_valid, ld_cache_hit, load_miss, mem2_commits;
    assign ld_valid     = mem2_is_load & ~mem2_fault;
    assign ld_cache_hit = i_dcache_hit & i_translate_cacheable;
    assign load_miss    = ld_valid & ~ld_cache_hit;
    assign mem2_commits = i_valid & ~load_miss;

    logic [31:0] lc_complete_data;
    logic [1:0]  lc_complete_size, lc_complete_addr_lo;
    logic        lc_complete_sign;
    penumbra3_load_complete #(
        .IDX_BITS (SB_IDX_W)
    ) u_load_complete (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_load_valid       (ld_valid),
        .i_cache_hit        (ld_cache_hit),
        .i_load_dest        (i_payload.phys_dst),
        .i_load_size        (i_bundle.mem_size),
        .i_load_addr_lo     (i_payload.value[1:0]),
        .i_load_sign        (i_bundle.sign_ext),
        .i_load_paddr       (i_translate_paddr),
        .i_load_cacheable   (i_translate_cacheable),
        .i_fill_done        (i_fill_done),
        .i_fill_data        (i_fill_data),
        .i_fill_fault       (i_fill_fault),
        .o_load_pending     (o_load_pending),
        .o_launch_fill      (o_launch_fill),
        .o_fill_paddr       (o_fill_paddr),
        .o_fill_cacheable   (o_fill_cacheable),
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

    // A store writes through in MEM2 on a clean, retiring, fault-free slot.
    assign o_dcache_paddr   = i_translate_paddr;
    assign o_dcache_wdata   = i_store_wdata;
    assign o_dcache_byte_en = i_byte_en;
    assign o_dcache_we      = mem2_is_store & ~mem2_fault & mem2_fire;

    // MEM2->EX forward (EX's two-ahead / load-use leg): a resolved,
    // non-faulting, committing result. A missing load is not yet resolved, so
    // mem2_commits already excludes it.
    assign o_fwd_result = wb_value;
    assign o_fwd_dst    = i_payload.phys_dst;
    assign o_fwd_we     = i_payload.phys_dst_we;
    assign o_fwd_valid  = mem2_commits & ~mem2_fault & i_payload.phys_dst_we;

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

    bcause_e mem2_bcause;
    assign mem2_bcause = (load_miss & mem2_is_load) ? BCAUSE_LOAD
                       : mem2_is_store              ? BCAUSE_STORE
                       :                              i_bcause;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_wb_valid  <= 1'b0;
            o_wb_bcause <= BCAUSE_FLUSH;
        end else if (~i_hold) begin
            o_wb_valid  <= i_bubble ? 1'b0 : mem2_commits;
            o_wb_bcause <= i_bubble ? BCAUSE_FLUSH : mem2_bcause;
            if (~i_bubble & mem2_commits) begin
                o_wb_bundle  <= i_bundle;
                o_wb_payload <= wb_payload_d;
            end
        end
    end

    // -- Assertions (sim-only; Verilator --assert) ----------------
    // A missing load never commits to WB -- it is evicted and completes later.
    always_comb
        assert (!(load_miss && mem2_commits))
            else $error("penumbra3_mem2_stage: a missing load committed to WB");

    // A protection fault implies a TLB hit (the verdict module guarantees it).
    always_comb
        assert (!prot_f || i_translate_hit)
            else $error("penumbra3_mem2_stage: protection fault without a TLB hit");

endmodule
