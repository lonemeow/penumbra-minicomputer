// Unit-test wrapper for penumbra3_wb_stage.
//
// WB reads only two bundle fields (dst_sel, flags_updater) and the resolved
// payload, so this drives those flat rather than decoding an instruction word:
// the wrapper assembles the ctrl_bundle_t / dpath_payload_t the MEM2/WB register
// would present and fans WB's write strobes back out flat for the C++ driver.
// The two-cycle divmul dual write is exercised by holding the inputs across a
// tick (the spine's freeze keeps the same slot at WB for both cycles).
module penumbra3_wb_stage_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // -- Slot in (bundle fields WB consumes + the full payload, flat) --
    input  logic        i_valid,
    input  logic [2:0]  i_bcause,
    input  logic [3:0]  i_dst_sel,       // bundle.dst_sel (SPR# for a WRSPR)
    input  logic        i_flags_updater, // bundle.flags_updater

    input  logic [SB_IDX_W-1:0] i_phys_dst,
    input  logic                i_phys_dst_we,
    input  logic [31:0]         i_value,
    input  logic [3:0]          i_flags,
    input  logic [SB_IDX_W-1:0] i_phys_dst_aux,
    input  logic                i_phys_dst_aux_we,
    input  logic [31:0]         i_value_aux,
    input  logic [31:0]         i_pc,
    input  logic                i_fault_pending,
    input  logic [3:0]          i_fault_vec,
    input  logic [31:0]         i_fault_vaddr,
    input  logic [31:0]         i_fault_status,

    // -- WB outputs (flattened) -----------------------------------
    output logic                o_local_stall,
    output logic                o_insn_committed,
    output logic [2:0]          o_bcause,

    output logic [SB_IDX_W-1:0] o_regfile_idx,
    output logic [31:0]         o_regfile_data,
    output logic                o_regfile_we,

    output logic                o_spr_we,
    output logic [3:0]          o_spr_sel,
    output logic [31:0]         o_spr_value,

    output logic                o_flag_we,
    output logic [3:0]          o_flag_value,

    output logic                o_fault_commit,
    output logic [3:0]          o_fault_vec,
    output logic [31:0]         o_fault_pc,
    output logic [31:0]         o_fault_vaddr,
    output logic [31:0]         o_fault_status
);

    // -- Bundle + payload assembly --------------------------------
    ctrl_bundle_t   bundle;
    dpath_payload_t payload;
    always_comb begin
        bundle               = '0;
        bundle.dst_sel       = i_dst_sel;
        bundle.flags_updater = i_flags_updater;

        payload                 = '0;
        payload.value           = i_value;
        payload.value_aux       = i_value_aux;
        payload.flags           = i_flags;
        payload.phys_dst        = i_phys_dst;
        payload.phys_dst_we     = i_phys_dst_we;
        payload.phys_dst_aux    = i_phys_dst_aux;
        payload.phys_dst_aux_we = i_phys_dst_aux_we;
        payload.pc              = i_pc;
        payload.fault_pending   = i_fault_pending;
        payload.fault_vec       = i_fault_vec;
        payload.fault_vaddr     = i_fault_vaddr;
        payload.fault_status    = i_fault_status;
    end

    bcause_e bcause_out;

    penumbra3_wb_stage u_wb (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .i_bundle         (bundle),
        .i_payload        (payload),
        .i_valid          (i_valid),
        .i_bcause         (bcause_e'(i_bcause)),
        .o_local_stall    (o_local_stall),
        .o_insn_committed (o_insn_committed),
        .o_bcause         (bcause_out),
        .o_regfile_idx    (o_regfile_idx),
        .o_regfile_data   (o_regfile_data),
        .o_regfile_we     (o_regfile_we),
        .o_spr_we         (o_spr_we),
        .o_spr_sel        (o_spr_sel),
        .o_spr_value      (o_spr_value),
        .o_flag_we        (o_flag_we),
        .o_flag_value     (o_flag_value),
        .o_fault_commit   (o_fault_commit),
        .o_fault_vec      (o_fault_vec),
        .o_fault_pc       (o_fault_pc),
        .o_fault_vaddr    (o_fault_vaddr),
        .o_fault_status   (o_fault_status)
    );

    assign o_bcause = bcause_out;

endmodule
