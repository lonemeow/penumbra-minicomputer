// penumbra4_core — the gen4 core: its stages, their units and its storage.
//
// Only connects (doc/internals/penumbra4/structure.md, "The core"): every
// decision belongs to a stage or a unit. The L1s and the MMU are the
// machine's; the core reaches them through the fetch side's ports and the
// data side's.
//
// A redirect's kill: execute's clears what is behind the transfer, the
// queue, IF2 and the op issuing; commit's clears every stage.

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

    // ── Observability ────────────────────────────────────────────
    output reg_write_t [1:0] o_reg_write,
    output logic [1:0]       o_retire_count,
    output logic             o_prog_end
);

    localparam int QUEUE_DEPTH = LINE_WORDS;
    localparam int HEAD_WIDTH  = 3;

    // ── Between the stages ───────────────────────────────────────
    commit_redirect_t commit_redirect;
    logic             commit_kill;
    redirect_t        execute_redirect;
    relaunch_t        relaunch;
    fetch_slot_t      if2_slot;
    logic             front_kill;

    logic                                queue_room;
    logic                                queue_enq;
    op_t         [LINE_WORDS-1:0]        queue_entries;
    line_count_t                         queue_enq_count;
    op_t         [HEAD_WIDTH-1:0]        queue_head;
    logic        [HEAD_WIDTH-1:0]        queue_head_valid;
    logic [$clog2(HEAD_WIDTH+1)-1:0]     queue_taken;

    pair_t            issue_pair, execute_pair, launch_pair, resolve_pair, landing_pair;
    logic             execute_hold, launch_hold, resolve_hold, commit_hold;
    logic             back_kill;

    phys_reg_t [3:0]  read_phys;
    word_t     [3:0]  read_value;
    reg_write_t [1:0] reg_write;
    status_write_t    status_write;
    sr_t              sr;
    word_t            epc, esr;
    word_t [3:0]      scratch;

    assign front_kill = commit_kill || execute_redirect.valid;
    assign back_kill  = commit_kill;

    // ── Front end ────────────────────────────────────────────────
    penumbra4_fetch #(
        .RESET_PC (RESET_PC)
    ) u_fetch (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_commit_redirect  (commit_redirect),
        .i_execute_redirect (execute_redirect),
        .i_relaunch         (relaunch),
        .i_queue_room       (queue_room),
        .i_icache_busy      (i_icache_busy),
        .i_icache_done      (i_icache_done),
        .i_icache_word      (i_icache_access_word),
        .o_launch           (o_fetch_launch),
        .o_launch_vaddr     (o_fetch_vaddr),
        .o_vector_req       (o_icache_vector_req),
        .o_vector_addr      (o_icache_vector_addr),
        .o_if2              (if2_slot)
    );

    penumbra4_line u_line (
        .i_slot           (if2_slot),
        .i_kill           (front_kill),
        .i_hit            (i_icache_hit),
        .i_line           (i_icache_line),
        .i_access_line    (i_icache_access_line),
        .i_access_fault   (i_icache_access_fault),
        .i_paddr          (i_fetch_paddr),
        .i_cacheable      (i_fetch_cacheable),
        .i_fault          (i_fetch_fault),
        .i_queue_room     (queue_room),
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
        .i_flush      (front_kill),
        .i_enq        (queue_enq),
        .i_entries    (queue_entries),
        .i_enq_count  (queue_enq_count),
        .o_room       (queue_room),
        .o_head       (queue_head),
        .o_head_valid (queue_head_valid),
        .i_deq_count  (queue_taken)
    );

    // ── Back end ─────────────────────────────────────────────────
    penumbra4_issue #(
        .HEAD_WIDTH (HEAD_WIDTH),
        .LATER      (4)
    ) u_issue (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_head          (queue_head),
        .i_head_valid    (queue_head_valid),
        .o_taken         (queue_taken),
        .i_kill          (front_kill),
        .i_hold          (execute_hold),
        .i_in_flight     ({landing_pair, resolve_pair, launch_pair, execute_pair}),
        .i_sr            (sr),
        .i_epc           (epc),
        .i_esr           (esr),
        .i_scratch       (scratch),
        .o_read_phys     (read_phys),
        .i_read_value    (read_value),
        .o_pair          (issue_pair)
    );

    penumbra4_execute u_execute (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_pair     (issue_pair),
        .i_kill     (back_kill),
        .i_hold     (launch_hold),
        .o_hold     (execute_hold),
        .o_redirect (execute_redirect),
        .o_pair     (execute_pair)
    );

    penumbra4_mem_launch u_mem_launch (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (execute_pair),
        .i_kill         (back_kill),
        .i_hold         (resolve_hold),
        .o_hold         (launch_hold),
        .o_launch       (o_data_launch),
        .o_launch_vaddr (o_data_vaddr),
        .o_pair         (launch_pair)
    );

    penumbra4_mem_resolve u_mem_resolve (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (launch_pair),
        .i_kill         (back_kill),
        .i_hold         (commit_hold),
        .o_hold         (resolve_hold),
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
        .o_pair         (resolve_pair)
    );

    penumbra4_commit u_commit (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pair         (resolve_pair),
        .i_supervisor   (sr.s),
        .i_epc          (epc),
        .o_hold         (commit_hold),
        .o_landing      (landing_pair),
        .o_reg_write    (reg_write),
        .o_status_write (status_write),
        .o_redirect     (commit_redirect),
        .o_kill         (commit_kill),
        .o_retire_count (o_retire_count),
        .o_prog_end     (o_prog_end)
    );

    // ── Storage ──────────────────────────────────────────────────
    penumbra4_regfile u_regfile (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_read_phys  (read_phys),
        .o_read_value (read_value),
        .i_write      (reg_write)
    );

    penumbra4_status u_status (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_write   (status_write),
        .o_sr      (sr),
        .o_epc     (epc),
        .o_esr     (esr),
        .o_scratch (scratch)
    );

    assign o_reg_write = reg_write;

endmodule
