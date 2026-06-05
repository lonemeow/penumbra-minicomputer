// penumbra2_if2_stage — Penumbra/2 instruction fetch, second half.
//
// Specified by the IF2 section of doc/internals/penumbra2/pipeline-stages.md.
// Receives the IF1/IF2 register (this fetch's PC, next_PC, valid) and the
// instruction word the I-cache BRAM presents this cycle — the word for the
// address IF1 drove last cycle — and latches the IF2/ID register for ID under
// the back-pressure handshake.
//
// The full stage compares the BRAM tag against the TLB-provided paddr, detects
// I-side faults (TLB miss/protection, bus fault, alignment), and stalls on a
// cache miss. None of that exists yet: fetch is hit-always against a flat BRAM
// stand-in with no MMU, so the word is delivered unconditionally and no fault
// is raised. IF2's only stall source is back-pressure from ID (i_stall_in);
// when held, it freezes its output register and back-pressures IF1, which
// holds the PC and the address so the same word stays presented.

module penumbra2_if2_stage
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── IF1/IF2 register in ──────────────────────────────────────
    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,        // 0 = bubble in

    // ── Instruction word from the I-cache BRAM ───────────────────
    input  logic [31:0] i_ir,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic        i_stall_in,     // ID cannot accept this cycle
    output logic        o_stall,         // back-pressure to IF1

    // ── IF2/ID register out (to ID) ──────────────────────────────
    output logic [31:0] o_ir,
    output logic [31:0] o_pc,
    output logic [31:0] o_next_pc,
    output logic        o_valid,
    output logic        o_fault_pending,
    output logic [3:0]  o_fault_vec
);

    // ── Issue / back-pressure control ────────────────────────────
    // IF2 has no stall source of its own yet (hit-always, no miss path),
    // so it advances every cycle unless ID back-pressures it.
    logic advance, next_valid;

    always_comb begin
        if (i_stall_in) begin
            next_valid = o_valid;       // hold IF2/ID unchanged
            advance    = 1'b0;
            o_stall    = 1'b1;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
            o_stall    = 1'b0;
        end
    end

    // ── IF2/ID register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (advance) begin
                o_ir      <= i_ir;
                o_pc      <= i_pc;
                o_next_pc <= i_next_pc;
            end
        end
    end

    // No I-side fault path yet (no MMU / tag compare): a fetched word never
    // faults. The fields exist so the IF2/ID boundary matches the spec and
    // ID's input, ready for the fault path to drive them later.
    assign o_fault_pending = 1'b0;
    assign o_fault_vec     = 4'd0;

endmodule
