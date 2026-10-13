// ULX3S board top (Penumbra/3 probe P0.5) -- L1 tag compare behind the TLB.
//
// A synthesis instrument, not a usable machine. It puts the two resolve
// cycles that chain an L1's physical-tag compare behind the BRAM TLB in
// front of nextpnr, each ending at the flops the core gives it:
//
//   MEM2: TLB and L1 block RAM outputs -> frame candidates -> per-way tag
//     compare -> way select -> word -> sub-word extract -> the MEM2/WB
//     register, the load-completion hold buffer, and the store's byte
//     merge into the L1's registered write.
//   IF2: the same compare on the I-side -> the way select of bundles each
//     way decoded from its own word -> the fetch FIFO; and each way's
//     pre-decode, the direction counter and the return stack's top -> the
//     predicted redirect -> the PC register.
//
// Everything memory-shaped is the real gen3 RTL: penumbra3_mmu (both
// translate copies, the pinned TLB, bypass), penumbra3_l1 per side,
// penumbra3_mem1_stage and
// penumbra3_mem2_stage, penumbra3_decode and penumbra3_fetch_buffer, and the
// predictor pieces. What the probe models is the stimulus (an LFSR standing
// in for EX, ID, the bus and the sysreg writes) and the fetch control.
//
// The fetch control follows the replay contract, not today's IF1/IF2
// stall: an I-cache miss sets a flop and next cycle steers IF1 back to the
// missed PC while the fill runs, so the hit never reaches IF1's hold. The
// hold is FIFO-full or a registered fill in progress, both flops. A
// predicted redirect steers the PC in the cycle IF2 presents the transfer
// and bubbles the fetch behind it; EX's redirect and the replay, both
// registered, outrank it.
//
// Two rules keep IF2's cones short. The word->bundle decode runs once per
// way, on each way's word straight off the block RAM, beside the TLB match
// and the tag compare; the hit only selects a finished bundle. And the
// predicted redirect waits on neither the translation fault nor the
// cacheable hit: a slot that faults traps from registered state, and one
// that misses is replayed, so steering the next fetch from its word costs
// at most that wrong-path fetch. The return stack, which must not take a
// wrong-path push or pop, updates a cycle after its transfer enqueues.
//
// Every output folds onto the LEDs through registers, so no keep-alive XOR
// lands on a measured cone. Clocking is the 25 MHz crystal directly, one
// clean domain for comparable fmax.
module ulx3s_penumbra3_probe_l1tag_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en      // LOW = hold ESP32 in reset
);
    import penumbra_pkg::*;
    import penumbra3_pkg::*;

    localparam int TLB_WAYS  = 2;
    localparam int FRAMES    = TLB_WAYS + 1;
    localparam int I_WAYS    = 2;         // 8 KB L1-I
    localparam int D_WAYS    = 2;         // 8 KB L1-D
    localparam int LINE_BYTES = 16;
    localparam int LINE_WORDS = LINE_BYTES / 4;
    localparam int RAS_DEPTH = 8;
    localparam int DIR_ENTRIES = 1024;
    localparam int FBUF_W = $bits(ctrl_bundle_t) + 32 + 32 + 1 + 4 + 32;

    // Keep-alive folds several unrelated vectors; their unread bits are fine.
    /* verilator lint_off UNUSEDSIGNAL */
    /* verilator lint_off PINCONNECTEMPTY */

    assign wifi_en = 1'b0;

    logic clk;
    assign clk = clk_25mhz;

    // ── Reset: power-on counter + btn[1] (FIRE1) ─────────────────
    logic btn1_sync1_q, btn1_sync2_q;
    always_ff @(posedge clk) begin
        btn1_sync1_q <= btn[1];
        btn1_sync2_q <= btn1_sync1_q;
    end

    /* verilator lint_off PROCASSINIT */
    logic [18:0] rst_cnt_q = '0;
    /* verilator lint_on PROCASSINIT */
    logic rst;
    always_ff @(posedge clk) begin
        if (btn1_sync2_q)        rst_cnt_q <= '0;
        else if (!rst_cnt_q[18]) rst_cnt_q <= rst_cnt_q + 1;
    end
    assign rst = !rst_cnt_q[18];

    // ── Stimulus: a 64-bit maximal LFSR, perturbed by a button ───
    logic [63:0] lfsr_q;
    always_ff @(posedge clk) begin
        if (rst)
            lfsr_q <= 64'h1;
        else
            lfsr_q <= {lfsr_q[62:0],
                       lfsr_q[63] ^ lfsr_q[62] ^ lfsr_q[60] ^ lfsr_q[59] ^ btn[2]};
    end

    // A few sparse events, each a registered AND of LFSR bits.
    logic ex_redirect_q, sys_we_q, sys_re_q, train_q, bubble_q;
    logic [31:0] ex_target_q, sys_wdata_q;
    logic [3:0]  sys_reg_q;
    logic        user_q, force_bypass_q, fault_commit_q, deq_ready_q;
    always_ff @(posedge clk) begin
        ex_redirect_q  <= !rst && lfsr_q[3] && lfsr_q[17] && lfsr_q[29];
        ex_target_q    <= {lfsr_q[45:16], 2'b00};
        sys_we_q       <= !rst && lfsr_q[5] && lfsr_q[33] && lfsr_q[51];
        sys_re_q       <= lfsr_q[6] && lfsr_q[34];
        sys_reg_q      <= lfsr_q[12:9];
        sys_wdata_q    <= lfsr_q[55:24];
        train_q        <= lfsr_q[8] && lfsr_q[26];
        bubble_q       <= lfsr_q[13] && lfsr_q[37] && lfsr_q[49];
        user_q         <= lfsr_q[14];
        force_bypass_q <= lfsr_q[15] && lfsr_q[39];
        fault_commit_q <= lfsr_q[16] && lfsr_q[42];
        deq_ready_q    <= lfsr_q[18];
    end

    // ══════════════════════════════════════════════════════════
    // MMU: both translate copies, the pinned TLB, bypass
    // ══════════════════════════════════════════════════════════
    logic        fetch_hold, d_hold;
    logic [31:0] pc_q;
    logic        tr_en;
    logic [31:0] tr_vaddr;
    logic [2:0]  tr_acc;

    logic [31:0]                i_paddr, d_paddr, i_fault_status, sys_rdata;
    logic                       i_cacheable, i_fault;
    logic                       d_cacheable, d_hit, d_miss_fault, d_prot_fault;
    logic [TLB_WAYS:0][19:0]    i_frame, d_frame;
    logic [TLB_WAYS:0]          i_frame_sel, d_frame_sel;

    penumbra3_mmu #(
        .WAYS (TLB_WAYS)
    ) u_mmu (
        .i_clk            (clk),
        .i_rst            (rst),
        .i_i_lookup_en    (1'b1),
        .i_i_vaddr        (pc_q),
        .i_i_user_mode    (user_q),
        .i_i_force_bypass (force_bypass_q),
        .i_i_hold         (fetch_hold),
        .o_i_paddr        (i_paddr),
        .o_i_cacheable    (i_cacheable),
        .o_i_fault        (i_fault),
        .o_i_fault_status (i_fault_status),
        .o_i_frame        (i_frame),
        .o_i_frame_sel    (i_frame_sel),
        .i_d_lookup_en    (tr_en),
        .i_d_vaddr        (tr_vaddr),
        .i_d_access_type  (tr_acc),
        .i_d_user_mode    (user_q),
        .i_d_hold         (d_hold),
        .o_d_paddr        (d_paddr),
        .o_d_cacheable    (d_cacheable),
        .o_d_hit          (d_hit),
        .o_d_miss_fault   (d_miss_fault),
        .o_d_prot_fault   (d_prot_fault),
        .o_d_frame        (d_frame),
        .o_d_frame_sel    (d_frame_sel),
        .i_fault_commit   (fault_commit_q),
        .i_fault_vaddr    (ex_target_q),
        .i_fault_status   (sys_wdata_q),
        .i_sys_reg        (sys_reg_q),
        .i_sys_wdata      (sys_wdata_q),
        .i_sys_we         (sys_we_q),
        .i_sys_re         (sys_re_q),
        .o_sys_rdata      (sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // D side: EX/MEM1 register -> MEM1 launch -> MEM2 resolve
    // ══════════════════════════════════════════════════════════
    // The EX/MEM1 register, loaded from the LFSR as EX would load it.
    ctrl_bundle_t   exm_bundle_d, exm_bundle_q;
    dpath_payload_t exm_payload_d, exm_payload_q;
    logic [31:0]    exm_store_data_q;
    logic           exm_valid_q;
    penumbra3_decode u_exm_decode (
        .i_ir     (lfsr_q[31:0]),
        .o_bundle (exm_bundle_d)
    );
    always_comb begin
        exm_payload_d               = '0;
        exm_payload_d.value         = lfsr_q[63:32];
        exm_payload_d.phys_dst      = lfsr_q[SB_IDX_W+19:20];
        exm_payload_d.phys_dst_we   = lfsr_q[27];
        exm_payload_d.pc            = {lfsr_q[47:18], 2'b00};
        exm_payload_d.fault_pending = lfsr_q[28] && lfsr_q[44];
        exm_payload_d.fault_vec     = lfsr_q[59:56];
        exm_payload_d.fault_status  = lfsr_q[53:22];
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            exm_valid_q <= 1'b0;
        end else if (!d_hold) begin
            exm_bundle_q     <= exm_bundle_d;
            exm_payload_q    <= exm_payload_d;
            exm_store_data_q <= {lfsr_q[15:0], lfsr_q[47:32]};
            exm_valid_q      <= lfsr_q[4];
        end
    end

    logic           dc_en;
    logic [31:0]    dc_vaddr;
    ctrl_bundle_t   mem2_bundle;
    dpath_payload_t mem2_payload;
    logic [31:0]    mem2_store_wdata;
    logic [3:0]     mem2_byte_en;
    logic           mem2_align_fault, mem2_valid;
    bcause_e        mem2_bcause;

    penumbra3_mem1_stage u_mem1 (
        .i_clk                (clk),
        .i_rst                (rst),
        .i_bundle             (exm_bundle_q),
        .i_payload            (exm_payload_q),
        .i_store_data         (exm_store_data_q),
        .i_valid              (exm_valid_q),
        .i_bcause             (BCAUSE_NONE),
        .i_hold               (d_hold),
        .i_bubble             (bubble_q),
        .o_translate_en       (tr_en),
        .o_translate_vaddr    (tr_vaddr),
        .o_translate_acc_type (tr_acc),
        .o_dcache_en          (dc_en),
        .o_dcache_vaddr       (dc_vaddr),
        .o_sys_re             (),
        .o_sys_dev            (),
        .o_sys_reg            (),
        .o_mem2_bundle        (mem2_bundle),
        .o_mem2_payload       (mem2_payload),
        .o_mem2_store_wdata   (mem2_store_wdata),
        .o_mem2_byte_en       (mem2_byte_en),
        .o_mem2_align_fault   (mem2_align_fault),
        .o_mem2_valid         (mem2_valid),
        .o_mem2_bcause        (mem2_bcause)
    );

    // ── L1-D ─────────────────────────────────────────────────────
    logic                        dl1_hit, dl1_en;
    logic [31:0]                 dl1_rdata, dl1_sys_rdata;
    logic [$clog2(D_WAYS)-1:0]   dl1_victim;

    logic                        dfill_we;
    logic [31:0]                 dfill_paddr_q;
    logic [$clog2(D_WAYS)-1:0]   dfill_way_q, dvictim_q;
    logic [LINE_WORDS-1:0][31:0] fill_line;
    logic                        dc_we;
    logic [31:0]                 dc_wdata;
    logic [3:0]                  dc_byte_en;

    always_comb
        for (int c = 0; c < LINE_WORDS; c++)
            fill_line[c] = lfsr_q[31:0] ^ {lfsr_q[63:56], 24'(c)};

    penumbra3_l1 #(
        .WAYS       (D_WAYS),
        .LINE_BYTES (LINE_BYTES),
        .FRAMES     (FRAMES)
    ) u_dl1 (
        .i_clk           (clk),
        .i_rst           (rst),
        .i_launch        (dc_en && !d_hold),
        .i_offset        (dc_vaddr[11:0]),
        .i_frame         (d_frame),
        .i_frame_sel     (d_frame_sel),
        .o_way_hit       (),
        .o_way_word      (),
        .o_hit           (dl1_hit),
        .o_word          (dl1_rdata),
        .o_victim_way    (dl1_victim),
        .i_store         (dc_we),
        .i_store_wdata   (dc_wdata),
        .i_store_byte_en (dc_byte_en),
        .i_count_read    (mem2_valid && mem2_bundle.mem_op == MEM_LOAD && d_cacheable),
        .i_count_write   (mem2_valid && mem2_bundle.mem_op == MEM_STORE && d_cacheable),
        .i_fill_we       (dfill_we),
        .i_fill_paddr    (dfill_paddr_q),
        .i_fill_way      (dfill_way_q),
        .i_fill_line     (fill_line),
        .i_sys_reg       (sys_reg_q),
        .i_sys_wdata     (sys_wdata_q),
        .i_sys_we        (sys_we_q),
        .i_sys_re        (sys_re_q),
        .o_sys_rdata     (dl1_sys_rdata),
        .o_enabled       (dl1_en)
    );

    // ── MEM2: resolve, hold buffer, MEM2/WB register ─────────────
    logic           load_pending, launch_fill, fill_cacheable, complete, complete_fault;
    logic [31:0]    fill_paddr, complete_value;
    logic [SB_IDX_W-1:0] complete_dest;
    logic [31:0]    dc_paddr;
    ctrl_bundle_t   wb_bundle;
    dpath_payload_t wb_payload;
    logic           wb_valid;
    bcause_e        wb_bcause;
    logic           dfill_done;

    penumbra3_mem2_stage u_mem2 (
        .i_clk                  (clk),
        .i_rst                  (rst),
        .i_bundle               (mem2_bundle),
        .i_payload              (mem2_payload),
        .i_store_wdata          (mem2_store_wdata),
        .i_byte_en              (mem2_byte_en),
        .i_align_fault          (mem2_align_fault),
        .i_valid                (mem2_valid),
        .i_bcause               (mem2_bcause),
        .i_user_mode            (user_q),
        .i_translate_paddr      (d_paddr),
        .i_translate_cacheable  (d_cacheable),
        .i_translate_hit        (d_hit),
        .i_translate_miss_fault (d_miss_fault),
        .i_translate_prot_fault (d_prot_fault),
        .i_dcache_hit           (dl1_hit),
        .i_dcache_rdata         (dl1_rdata),
        .o_dcache_paddr         (dc_paddr),
        .o_dcache_we            (dc_we),
        .o_dcache_wdata         (dc_wdata),
        .o_dcache_byte_en       (dc_byte_en),
        .i_sys_rdata            (sys_rdata),
        .o_launch_fill          (launch_fill),
        .o_fill_paddr           (fill_paddr),
        .o_fill_cacheable       (fill_cacheable),
        .i_fill_done            (dfill_done),
        .i_fill_data            (lfsr_q[63:32]),
        .i_fill_fault           (lfsr_q[1] && lfsr_q[21] && lfsr_q[43]),
        .o_load_pending         (load_pending),
        .o_complete             (complete),
        .o_complete_dest        (complete_dest),
        .o_complete_value       (complete_value),
        .o_complete_fault       (complete_fault),
        .o_fwd_result           (),
        .o_fwd_dst              (),
        .o_fwd_we               (),
        .o_fwd_valid            (),
        .i_launch_hold          (d_hold),
        .i_hold                 (1'b0),
        .i_bubble               (bubble_q),
        .o_wb_bundle            (wb_bundle),
        .o_wb_payload           (wb_payload),
        .o_wb_valid             (wb_valid),
        .o_wb_bcause            (wb_bcause)
    );

    assign d_hold = load_pending;

    // ── The fill, modeled: done after an LFSR-timed wait ─────────
    // The fill launches a cycle after its miss resolved, so the victim
    // from that resolve is a register by then.
    logic dfill_busy_q, dfill_cacheable_q;
    assign dfill_done = dfill_busy_q && lfsr_q[40];
    assign dfill_we   = dfill_done && dfill_cacheable_q && dl1_en;
    always_ff @(posedge clk) begin
        if (rst)              dfill_busy_q <= 1'b0;
        else if (launch_fill) dfill_busy_q <= 1'b1;
        else if (dfill_done)  dfill_busy_q <= 1'b0;
        dvictim_q   <= dl1_victim;
        if (launch_fill) begin
            dfill_paddr_q     <= fill_paddr;
            dfill_cacheable_q <= fill_cacheable;
            dfill_way_q       <= dvictim_q;
        end
    end

    // ══════════════════════════════════════════════════════════
    // I side: PC -> IF1 launch -> IF2 resolve, pre-decode, enqueue
    // ══════════════════════════════════════════════════════════
    logic [31:0] if2_pc_q;
    logic        if2_valid_q;
    logic        replay_q, ifill_busy_q, fbuf_enq_ready;
    logic [31:0] replay_pc_q;

    // The hold: the IF2 slot waits on FIFO room, or a fill is in flight.
    assign fetch_hold = (if2_valid_q && !fbuf_enq_ready) || ifill_busy_q;

    // ── L1-I ─────────────────────────────────────────────────────
    logic [I_WAYS-1:0]         il1_way_hit;
    logic [I_WAYS-1:0][31:0]   il1_word;
    logic                      il1_hit, il1_en, il1_count_read;
    logic [31:0]               il1_sys_rdata;
    logic [$clog2(I_WAYS)-1:0] il1_victim;

    logic        ifill_we;
    logic [31:0] ifill_paddr_q;
    logic [$clog2(I_WAYS)-1:0] ifill_way_q;

    penumbra3_l1 #(
        .WAYS       (I_WAYS),
        .LINE_BYTES (LINE_BYTES),
        .FRAMES     (FRAMES)
    ) u_il1 (
        .i_clk           (clk),
        .i_rst           (rst),
        .i_launch        (!fetch_hold),
        .i_offset        (pc_q[11:0]),
        .i_frame         (i_frame),
        .i_frame_sel     (i_frame_sel),
        .o_way_hit       (il1_way_hit),
        .o_way_word      (il1_word),
        .o_hit           (il1_hit),
        .o_word          (),                // each way's word decodes before the select
        .o_victim_way    (il1_victim),
        .i_store         (1'b0),
        .i_store_wdata   ('0),
        .i_store_byte_en ('0),
        .i_count_read    (il1_count_read),
        .i_count_write   (1'b0),
        .i_fill_we       (ifill_we),
        .i_fill_paddr    (ifill_paddr_q),
        .i_fill_way      (ifill_way_q),
        .i_fill_line     (fill_line),
        .i_sys_reg       (sys_reg_q),
        .i_sys_wdata     (sys_wdata_q),
        .i_sys_we        (sys_we_q),
        .i_sys_re        (sys_re_q),
        .o_sys_rdata     (il1_sys_rdata),
        .o_enabled       (il1_en)
    );

    // ── Predictor: direction table, return stack, per-way pre-decode ─
    logic [1:0]  dir_counter;
    logic [31:0] ras_top;
    logic        ras_push, ras_pop;

    penumbra3_direction_table #(
        .ENTRIES (DIR_ENTRIES)
    ) u_dir (
        .i_clk           (clk),
        .i_read_en       (!fetch_hold),
        .i_fetch_pc      (pc_q),
        .o_counter       (dir_counter),
        .i_train_en      (train_q),
        .i_train_index   (lfsr_q[61:52]),
        .i_train_counter (lfsr_q[63:62])
    );

    penumbra3_return_stack #(
        .DEPTH (RAS_DEPTH)
    ) u_ras (
        .i_clk       (clk),
        .i_rst       (rst),
        .i_push      (ras_push),
        .i_push_addr (ras_push_addr_q),
        .i_pop       (ras_pop),
        .o_top       (ras_top)
    );

    logic [I_WAYS-1:0]       pd_direct, pd_cond, pd_call, pd_return;
    logic [I_WAYS-1:0][31:0] pd_target;
    for (genvar gw = 0; gw < I_WAYS; gw++) begin : g_predecode
        penumbra3_predecode u_predecode (
            .i_word        (il1_word[gw]),
            .i_pc          (if2_pc_q),
            .o_direct      (pd_direct[gw]),
            .o_conditional (pd_cond[gw]),
            .o_call        (pd_call[gw]),
            .o_return      (pd_return[gw]),
            .o_target      (pd_target[gw])
        );
    end

    // ── IF2: enqueue, predicted redirect, replay ─────────────────
    // A slot EX's redirect or the replay steers past is wrong-path. The
    // redirect needs only a way to hit (the AND-OR below) and FIFO room.
    logic slot_live, ihit, enq_valid, enq_fire, word_enq;
    assign slot_live = if2_valid_q && !replay_q && !ex_redirect_q;
    assign il1_count_read = slot_live && i_cacheable;
    assign ihit      = il1_hit && i_cacheable;
    assign enq_valid = slot_live && (i_fault || ihit);
    assign enq_fire  = enq_valid && fbuf_enq_ready;
    assign word_enq  = enq_fire && !i_fault;

    logic [I_WAYS-1:0]       way_redirect;
    logic [I_WAYS-1:0][31:0] way_target;
    always_comb
        for (int w = 0; w < I_WAYS; w++) begin
            way_redirect[w] = (pd_direct[w] && (!pd_cond[w] || dir_counter[1]))
                           || pd_return[w];
            way_target[w]   = pd_return[w] ? ras_top : pd_target[w];
        end

    logic        pred_redirect;
    logic [31:0] pred_target;
    always_comb begin
        pred_target = '0;
        for (int w = 0; w < I_WAYS; w++)
            pred_target |= {32{il1_way_hit[w]}} & way_target[w];
    end
    assign pred_redirect = slot_live && fbuf_enq_ready && |(il1_way_hit & way_redirect);

    logic        ras_push_q, ras_pop_q;
    logic [31:0] ras_push_addr_q;
    always_ff @(posedge clk) begin
        ras_push_q      <= !rst && word_enq && |(il1_way_hit & pd_call);
        ras_pop_q       <= !rst && word_enq && |(il1_way_hit & pd_return);
        ras_push_addr_q <= if2_pc_q + 32'd4;
    end
    assign ras_push = ras_push_q;
    assign ras_pop  = ras_pop_q;

    logic replay_d;
    assign replay_d = slot_live && !i_fault && !ihit;

    // ── PC and the IF1/IF2 register ──────────────────────────────
    always_ff @(posedge clk) begin
        if (rst)                pc_q <= 32'hFFFF_0000;
        else if (ex_redirect_q) pc_q <= ex_target_q;
        else if (replay_q)      pc_q <= replay_pc_q;
        else if (pred_redirect) pc_q <= pred_target;
        else if (!fetch_hold)   pc_q <= pc_q + 32'd4;
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            if2_valid_q <= 1'b0;
        end else if (ex_redirect_q || replay_q || pred_redirect) begin
            if2_valid_q <= 1'b0;
        end else if (!fetch_hold) begin
            if2_valid_q <= 1'b1;
            if2_pc_q    <= pc_q;
        end
    end

    // ── Replay and the I-fill, modeled like the D-side one ───────
    logic ifill_done, ifill_cacheable_q;
    assign ifill_done = ifill_busy_q && lfsr_q[50];
    assign ifill_we   = ifill_done && ifill_cacheable_q && il1_en;
    always_ff @(posedge clk) begin
        replay_q <= !rst && replay_d;
        if (replay_d) begin
            replay_pc_q       <= if2_pc_q;
            ifill_paddr_q     <= i_paddr;
            ifill_cacheable_q <= i_cacheable;
            ifill_way_q       <= il1_victim;
        end
        if (rst)             ifill_busy_q <= 1'b0;
        else if (replay_d)   ifill_busy_q <= 1'b1;
        else if (ifill_done) ifill_busy_q <= 1'b0;
    end

    // ── Word -> bundle decode, per way, into the fetch FIFO ──────
    localparam int BUNDLE_W = $bits(ctrl_bundle_t);
    logic [I_WAYS-1:0][BUNDLE_W-1:0] way_bundle;
    for (genvar gw = 0; gw < I_WAYS; gw++) begin : g_decode
        ctrl_bundle_t bundle;
        penumbra3_decode u_decode (
            .i_ir     (il1_word[gw]),
            .o_bundle (bundle)
        );
        assign way_bundle[gw] = bundle;
    end

    ctrl_bundle_t if2_bundle;
    always_comb begin
        logic [BUNDLE_W-1:0] selected;
        selected = '0;
        for (int w = 0; w < I_WAYS; w++)
            selected |= {BUNDLE_W{il1_way_hit[w]}} & way_bundle[w];
        if2_bundle = ctrl_bundle_t'(selected);
    end

    logic [3:0] if2_fault_vec;
    assign if2_fault_vec = i_fault ? fault_vec_of(i_fault_status[3:0]) : 4'd0;

    logic              fbuf_deq_valid;
    logic [FBUF_W-1:0] fbuf_deq_data;
    penumbra3_fetch_buffer #(
        .PAYLOAD_W (FBUF_W),
        .DEPTH     (2)
    ) u_fbuf (
        .i_clk       (clk),
        .i_rst       (rst),
        .i_flush     (ex_redirect_q),
        .i_enq_valid (enq_valid),
        .i_enq_data  ({if2_bundle, if2_pc_q, if2_pc_q + 32'd4,
                       i_fault_status, if2_fault_vec, i_fault}),
        .o_enq_ready (fbuf_enq_ready),
        .o_deq_valid (fbuf_deq_valid),
        .o_deq_data  (fbuf_deq_data),
        .i_deq_ready (deq_ready_q)
    );

    // ══════════════════════════════════════════════════════════
    // Keep-alive: fold registered outputs onto the LEDs
    // ══════════════════════════════════════════════════════════
    // Each fold starts at a register, so it is a separate off-cone path.
    logic [7:0] fold_q, led_q;
    always_ff @(posedge clk) begin
        fold_q[0] <= fbuf_deq_valid ^ (^fbuf_deq_data);
        fold_q[1] <= wb_valid ^ (^wb_payload) ^ (^wb_bundle) ^ (^wb_bcause);
        fold_q[2] <= complete ^ complete_fault ^ (^complete_dest) ^ (^complete_value);
        fold_q[3] <= launch_fill ^ (^fill_paddr);
        fold_q[4] <= ^pc_q;
        fold_q[5] <= ^ras_top;
        fold_q[6] <= ^{dl1_sys_rdata, il1_sys_rdata};
        fold_q[7] <= ^{ifill_paddr_q, replay_pc_q};
        led_q     <= fold_q;
    end
    assign led = led_q;

    /* verilator lint_on PINCONNECTEMPTY */
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
