// penumbra4_core — the gen4 core: its stages, their units and its storage.
//
// Only connects (doc/internals/penumbra4/structure.md, "The core"): every
// decision belongs to a stage or a unit. The L1s, the MMU and the sysreg
// devices are the machine's; the core reaches them through the fetch
// side's ports, the data side's and MEM1's sysreg request.
//
// A redirect's flush: EX's clears what is behind the transfer, the queue,
// IF2 and the op issuing; WB's clears every stage.

module penumbra4_core
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter vaddr_t RESET_PC = 32'hFFFF_0000
)(
    input  logic             i_clk,
    input  logic             i_rst,

    // ── The L1-I's lookups and accesses ──────────────────────────
    output logic             o_fetch_launch,
    output vaddr_t           o_fetch_vaddr,
    input  logic             i_icache_hit,
    input  line_t            i_icache_line,
    output logic             o_icache_miss_req,
    output paddr_t           o_icache_miss_addr,
    output logic             o_icache_miss_cacheable,
    output logic             o_icache_vector_req,
    output paddr_t           o_icache_vector_addr,
    input  logic             i_icache_busy,
    input  logic             i_icache_done,
    input  line_t            i_icache_access_line,
    input  word_t            i_icache_access_word,
    input  logic             i_icache_access_fault,

    // ── The MMU's fetch verdict ──────────────────────────────────
    input  paddr_t           i_fetch_paddr,
    input  logic             i_fetch_cacheable,
    input  fault_type_t      i_fetch_fault,

    // ── The L1-D's lookups and accesses ──────────────────────────
    output logic             o_data_launch,
    output vaddr_t           o_data_vaddr,
    input  logic             i_dcache_hit,
    input  word_t            i_dcache_word,
    output logic             o_dcache_req,
    output uncore_req_t      o_dcache_access,
    input  logic             i_dcache_done,
    input  word_t            i_dcache_access_word,
    input  logic             i_dcache_access_fault,

    // ── The MMU's data verdict ───────────────────────────────────
    input  paddr_t           i_data_paddr,
    input  logic             i_data_cacheable,
    input  fault_type_t      i_data_fault,

    // ── The sysreg devices ───────────────────────────────────────
    output sysreg_req_t      o_sysreg,
    input  sysreg_rsp_t      i_sysreg,

    // ── Observability ────────────────────────────────────────────
    output pair_t            o_committed,
    output sr_t              o_sr,
    output reg_write_t [1:0] o_reg_write,
    output logic [1:0]       o_retire_count,
    output logic             o_prog_end
);

    localparam int QUEUE_DEPTH = LINE_WORDS;
    localparam int HEAD_WIDTH  = 3;

    // ── Between the stages ───────────────────────────────────────
    wb_redirect_t     wb_redirect_q;
    logic             wb_flush_q;
    redirect_t        ex_redirect;
    relaunch_t        relaunch;
    fetch_slot_t      if2_slot_q;
    logic             front_flush;

    logic                                queue_room_q;
    logic                                queue_enq;
    op_t         [LINE_WORDS-1:0]        queue_entries;
    line_count_t                         queue_enq_count;
    op_t         [HEAD_WIDTH-1:0]        queue_head;
    logic        [HEAD_WIDTH-1:0]        queue_head_valid;
    logic [$clog2(HEAD_WIDTH+1)-1:0]     queue_taken;

    pair_t            id_pair_q, ex_pair_q, mem1_pair_q, mem2_pair_q, wb_pair_q;
    logic             ex_hold, mem1_hold, mem2_hold, wb_hold;

    phys_reg_t [3:0]  read_phys;
    word_t     [3:0]  read_value;
    reg_write_t [1:0] reg_write_q;
    status_write_t    status_write_q;
    sr_t              sr_q;
    word_t            epc_q, esr_q;
    word_t [3:0]      scratch_q;

    assign front_flush = wb_flush_q || ex_redirect.valid;

    // ── Front end ────────────────────────────────────────────────
    penumbra4_stage_if1 #(
        .RESET_PC (RESET_PC)
    ) u_if1 (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_wb_redirect      (wb_redirect_q),
        .i_ex_redirect      (ex_redirect),
        .i_relaunch         (relaunch),
        .i_queue_room       (queue_room_q),
        .i_icache_busy      (i_icache_busy),
        .i_icache_done      (i_icache_done),
        .i_icache_word      (i_icache_access_word),
        .o_launch           (o_fetch_launch),
        .o_launch_vaddr     (o_fetch_vaddr),
        .o_vector_req       (o_icache_vector_req),
        .o_vector_addr      (o_icache_vector_addr),
        .o_if2              (if2_slot_q)
    );

    penumbra4_stage_if2 u_if2 (
        .i_slot           (if2_slot_q),
        .i_flush          (front_flush),
        .i_hit            (i_icache_hit),
        .i_line           (i_icache_line),
        .i_access_line    (i_icache_access_line),
        .i_access_fault   (i_icache_access_fault),
        .i_paddr          (i_fetch_paddr),
        .i_cacheable      (i_fetch_cacheable),
        .i_fault          (i_fetch_fault),
        .i_queue_room     (queue_room_q),
        .o_enq            (queue_enq),
        .o_ops            (queue_entries),
        .o_count          (queue_enq_count),
        .o_miss_req       (o_icache_miss_req),
        .o_miss_addr      (o_icache_miss_addr),
        .o_miss_cacheable (o_icache_miss_cacheable),
        .o_relaunch       (relaunch)
    );

    penumbra4_fetch_queue #(
        .entry_t    (op_t),
        .DEPTH      (QUEUE_DEPTH),
        .ENQ_WIDTH  (LINE_WORDS),
        .HEAD_WIDTH (HEAD_WIDTH)
    ) u_queue (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_flush      (front_flush),
        .i_enq        (queue_enq),
        .i_entries    (queue_entries),
        .i_enq_count  (queue_enq_count),
        .o_room       (queue_room_q),
        .o_head       (queue_head),
        .o_head_valid (queue_head_valid),
        .i_deq_count  (queue_taken)
    );

    // ── Back end ─────────────────────────────────────────────────
    penumbra4_stage_id #(
        .HEAD_WIDTH (HEAD_WIDTH),
        .LATER      (4)
    ) u_id (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_head          (queue_head),
        .i_head_valid    (queue_head_valid),
        .o_taken         (queue_taken),
        .i_flush         (front_flush),
        .i_hold          (ex_hold),
        .i_in_flight     ({wb_pair_q, mem2_pair_q, mem1_pair_q, ex_pair_q}),
        .i_sr            (sr_q),
        .i_epc           (epc_q),
        .i_esr           (esr_q),
        .i_scratch       (scratch_q),
        .o_read_phys     (read_phys),
        .i_read_value    (read_value),
        .o_pair          (id_pair_q)
    );

    penumbra4_stage_ex u_ex (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_pair     (id_pair_q),
        .i_flush    (wb_flush_q),
        .i_hold     (mem1_hold),
        .o_hold     (ex_hold),
        .o_redirect (ex_redirect),
        .o_pair     (ex_pair_q)
    );

    penumbra4_stage_mem1 u_mem1 (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (ex_pair_q),
        .i_flush        (wb_flush_q),
        .i_hold         (mem2_hold),
        .o_hold         (mem1_hold),
        .o_launch       (o_data_launch),
        .o_launch_vaddr (o_data_vaddr),
        .o_sysreg       (o_sysreg),
        .o_pair         (mem1_pair_q)
    );

    penumbra4_stage_mem2 u_mem2 (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (mem1_pair_q),
        .i_flush        (wb_flush_q),
        .i_hold         (wb_hold),
        .o_hold         (mem2_hold),
        .i_paddr        (i_data_paddr),
        .i_cacheable    (i_data_cacheable),
        .i_fault        (i_data_fault),
        .i_hit          (i_dcache_hit),
        .i_word         (i_dcache_word),
        .o_access_req   (o_dcache_req),
        .o_access       (o_dcache_access),
        .i_access_done  (i_dcache_done),
        .i_access_word  (i_dcache_access_word),
        .i_access_fault (i_dcache_access_fault),
        .i_sysreg       (i_sysreg),
        .o_pair         (mem2_pair_q)
    );

    penumbra4_stage_wb u_wb (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (mem2_pair_q),
        .i_supervisor   (sr_q.s),
        .i_epc          (epc_q),
        .o_hold         (wb_hold),
        .o_committed    (wb_pair_q),
        .o_reg_write    (reg_write_q),
        .o_status_write (status_write_q),
        .o_redirect     (wb_redirect_q),
        .o_flush        (wb_flush_q),
        .o_retire_count (o_retire_count),
        .o_prog_end     (o_prog_end)
    );

    // ── Storage ──────────────────────────────────────────────────
    penumbra4_regfile u_regfile (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_read_phys  (read_phys),
        .o_read_value (read_value),
        .i_write      (reg_write_q)
    );

    penumbra4_status u_status (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_write   (status_write_q),
        .o_sr      (sr_q),
        .o_epc     (epc_q),
        .o_esr     (esr_q),
        .o_scratch (scratch_q)
    );

    assign o_committed = wb_pair_q;
    assign o_sr        = sr_q;
    assign o_reg_write = reg_write_q;

endmodule
