// Unit-test wrapper for penumbra3_decode.
//
// penumbra3_decode emits a single packed ctrl_bundle_t. Verilator would
// present that to the C++ testbench as one opaque wide vector, so this
// wrapper instantiates the decoder and fans the bundle out into named flat
// ports the testbench can read directly -- the bundle layout stays the one
// source of truth (penumbra3_pkg), and the test reads it by field.
module penumbra3_decode_test
    import penumbra3_pkg::*;
(
    input  logic [31:0] i_ir,

    output logic [3:0]  o_op_class,

    output logic [3:0]  o_src_a_sel,
    output logic        o_src_a_is_spr,
    output logic        o_src_a_en,
    output logic        o_src_a_is_pc,
    output logic [3:0]  o_src_b_sel,
    output logic        o_src_b_is_spr,
    output logic        o_src_b_en,
    output logic        o_src_b_is_pc,
    output logic [3:0]  o_dst_sel,
    output logic        o_dst_is_spr,
    output logic        o_dst_we,
    output logic [3:0]  o_dst_aux_sel,
    output logic        o_dst_aux_we,

    output logic [3:0]  o_alu_op,
    output logic [1:0]  o_divmul_op,
    output logic        o_a_from_pc,
    output logic        o_b_from_imm,
    output logic [31:0] o_imm,
    output logic [3:0]  o_cond,

    output logic        o_flags_updater,
    output logic        o_flags_reader,
    output logic        o_drain_commit,
    output logic        o_commit_wait,

    output logic [1:0]  o_mem_op,
    output logic [1:0]  o_mem_size,
    output logic        o_sign_ext,

    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,
    output logic [3:0]  o_spr_sel,

    output logic        o_is_trap,
    output logic        o_illegal,
    output logic        o_priv_op
);

    ctrl_bundle_t b;

    penumbra3_decode u_dec (
        .i_ir     (i_ir),
        .o_bundle (b)
    );

    assign o_op_class      = b.op_class;
    assign o_src_a_sel     = b.src_a_sel;
    assign o_src_a_is_spr  = b.src_a_is_spr;
    assign o_src_a_en      = b.src_a_en;
    assign o_src_a_is_pc   = b.src_a_is_pc;
    assign o_src_b_sel     = b.src_b_sel;
    assign o_src_b_is_spr  = b.src_b_is_spr;
    assign o_src_b_en      = b.src_b_en;
    assign o_src_b_is_pc   = b.src_b_is_pc;
    assign o_dst_sel       = b.dst_sel;
    assign o_dst_is_spr    = b.dst_is_spr;
    assign o_dst_we        = b.dst_we;
    assign o_dst_aux_sel   = b.dst_aux_sel;
    assign o_dst_aux_we    = b.dst_aux_we;
    assign o_alu_op        = b.alu_op;
    assign o_divmul_op     = b.divmul_op;
    assign o_a_from_pc     = b.a_from_pc;
    assign o_b_from_imm    = b.b_from_imm;
    assign o_imm           = b.imm;
    assign o_cond          = b.cond;
    assign o_flags_updater = b.flags_updater;
    assign o_flags_reader  = b.flags_reader;
    assign o_drain_commit  = b.drain_commit;
    assign o_commit_wait   = b.commit_wait;
    assign o_mem_op        = b.mem_op;
    assign o_mem_size      = b.mem_size;
    assign o_sign_ext      = b.sign_ext;
    assign o_sys_dev       = b.sys_dev;
    assign o_sys_reg       = b.sys_reg;
    assign o_spr_sel       = b.spr_sel;
    assign o_is_trap       = b.is_trap;
    assign o_illegal       = b.illegal;
    assign o_priv_op       = b.priv_op;

endmodule
