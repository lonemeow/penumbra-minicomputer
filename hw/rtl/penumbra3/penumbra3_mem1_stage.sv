// penumbra3_mem1_stage -- Penumbra/3 memory access launch (first half).
//
// Combinational launch logic plus the MEM1/MEM2 pipeline register. Reads the
// EX/MEM1 register EX latched and, for a memory access, derives the effective
// address, checks alignment, prepares the store lanes, and drives the launch
// ports: the address-translation query and the D-cache index (both resolved a
// cycle later in MEM2), and the RDSYS sysreg read. It then registers the
// access metadata into the MEM1/MEM2 boundary for penumbra3_mem2_stage.
//
// The translation and the cache live one level up (the spine owns them, since
// the TLB is shared coherently with the I-side copy); this stage only drives
// their launch side. i_hold freezes the MEM1/MEM2 register on a pipe stall;
// the spine drives the matching hold into the translation/cache so the verdict
// the held MEM2 slot reads stays pinned to its own access.

module penumbra3_mem1_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic           i_clk,
    input  logic           i_rst,

    // -- EX/MEM1 input: the instruction leaving EX ----------------
    input  ctrl_bundle_t   i_bundle,
    input  dpath_payload_t i_payload,        // value = the effective address for a mem op
    input  logic [31:0]    i_store_data,
    input  logic           i_valid,
    input  bcause_e        i_bcause,

    // -- Pipeline handshake ---------------------------------------
    input  logic           i_hold,           // freeze the MEM1/MEM2 register (pipe stall)
    input  logic           i_bubble,         // fault-commit flush from WB

    // -- Translation launch (spine drives dtranslate from these) --
    output logic           o_translate_en,
    output logic [31:0]    o_translate_vaddr,
    output logic [2:0]     o_translate_acc_type,

    // -- D-cache index launch (spine drives the cache) ------------
    output logic           o_dcache_en,
    output logic [31:0]    o_dcache_vaddr,

    // -- RDSYS sysreg read launch ---------------------------------
    output logic           o_sys_re,
    output logic [3:0]     o_sys_dev,
    output logic [3:0]     o_sys_reg,

    // -- MEM1/MEM2 register (to penumbra3_mem2_stage) -------------
    output ctrl_bundle_t   o_mem2_bundle,
    output dpath_payload_t o_mem2_payload,
    output logic [31:0]    o_mem2_store_wdata,
    output logic [3:0]     o_mem2_byte_en,
    output logic           o_mem2_align_fault,
    output logic           o_mem2_valid,
    output bcause_e        o_mem2_bcause
);

    // -- Access classification ------------------------------------
    logic mem1_is_load, mem1_is_store, mem1_is_mem, mem1_is_rdsys;
    assign mem1_is_load  = i_valid & (i_bundle.mem_op == MEM_LOAD)  & ~i_payload.fault_pending;
    assign mem1_is_store = i_valid & (i_bundle.mem_op == MEM_STORE) & ~i_payload.fault_pending;
    assign mem1_is_mem   = mem1_is_load | mem1_is_store;
    assign mem1_is_rdsys = i_valid & (i_bundle.op_class == OPC_RDSYS) & ~i_payload.fault_pending;

    // The effective address is EX's result for a memory op.
    logic [31:0] mem1_ea;
    assign mem1_ea = i_payload.value;

    // -- Alignment: byte ok; half needs EA[0]==0; word needs EA[1:0]==00 --
    logic mem1_misaligned;
    always_comb begin
        case (i_bundle.mem_size)
            MEM_SZ_WORD: mem1_misaligned = (mem1_ea[1:0] != 2'b00);
            MEM_SZ_HALF: mem1_misaligned = mem1_ea[0];
            default:     mem1_misaligned = 1'b0;
        endcase
    end
    logic mem1_align_fault;
    assign mem1_align_fault = mem1_is_mem & mem1_misaligned;

    // A clean (aligned) access launches a translation + a cache lookup; a flush
    // suppresses the launch.
    logic mem1_clean;
    assign mem1_clean = mem1_is_mem & ~mem1_misaligned & ~i_bubble;

    // -- Store lane replication + byte-enable (carried to MEM2) ----
    logic [31:0] mem1_store_wdata;
    byte_rep u_byte_rep (
        .i_data (i_store_data),
        .i_size (i_bundle.mem_size),
        .o_data (mem1_store_wdata)
    );
    logic [3:0] mem1_byte_en;
    always_comb begin
        case (i_bundle.mem_size)
            MEM_SZ_WORD: mem1_byte_en = 4'b1111;
            MEM_SZ_HALF: mem1_byte_en = mem1_ea[1] ? 4'b1100 : 4'b0011;
            MEM_SZ_BYTE: case (mem1_ea[1:0])
                             2'b00: mem1_byte_en = 4'b0001;
                             2'b01: mem1_byte_en = 4'b0010;
                             2'b10: mem1_byte_en = 4'b0100;
                             2'b11: mem1_byte_en = 4'b1000;
                         endcase
            default:     mem1_byte_en = 4'b0000;
        endcase
    end

    // -- Launch drives --------------------------------------------
    // The translation/cache hold is driven by the spine, so these are simply
    // "a real access is in MEM1 this cycle".
    assign o_translate_en       = mem1_clean;
    assign o_translate_vaddr    = mem1_ea;
    assign o_translate_acc_type = mem1_is_store ? ACC_WRITE : ACC_READ;
    assign o_dcache_en          = mem1_clean;
    assign o_dcache_vaddr       = mem1_ea;
    assign o_sys_re             = mem1_is_rdsys & ~i_bubble;
    assign o_sys_dev            = i_bundle.sys_dev;
    assign o_sys_reg            = i_bundle.sys_reg;

    // -- MEM1/MEM2 register ---------------------------------------
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_mem2_valid  <= 1'b0;
            o_mem2_bcause <= BCAUSE_FLUSH;
        end else if (~i_hold) begin
            o_mem2_valid  <= i_bubble ? 1'b0 : i_valid;
            o_mem2_bcause <= i_bubble ? BCAUSE_FLUSH : i_bcause;
            if (~i_bubble) begin
                o_mem2_bundle      <= i_bundle;
                o_mem2_payload     <= i_payload;
                o_mem2_store_wdata <= mem1_store_wdata;
                o_mem2_byte_en     <= mem1_byte_en;
                o_mem2_align_fault <= mem1_align_fault;
            end
        end
    end

    // -- Assertions (sim-only; Verilator --assert) ----------------
    // A reserved access size never reaches a real memory access.
    always_comb
        assert (!mem1_is_mem || i_bundle.mem_size != 2'b11)
            else $error("penumbra3_mem1_stage: reserved memory access size in MEM1");

endmodule
