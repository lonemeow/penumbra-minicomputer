// Penumbra ALU — Unified 32-bit compute unit with ARM-style flag generation
//
// The ALU is the single compute unit in the Penumbra datapath. It handles:
//   - Single-cycle ops: arithmetic, logic, shifts, pass-through (combinational)
//   - Multi-cycle MUL/DIV are handled by the divmul peer unit, not here (the
//     ALU outputs 0 for those op codes; datapath.sv muxes the divmul in).
//
// For C/C++ programmers:
//   Think of this as a function: result = alu(a, b, op)
//   Most ops complete instantly (combinational = no clock needed).
//   Some ops (multiply, divide) take many cycles — like a function that
//   sets a "busy" flag and you poll it until done.
//
//   `always_comb`  = a block that re-evaluates whenever inputs change
//                    (like a pure function with no side effects)
//   `always_ff`    = a block that updates only on clock edges
//                    (like a callback that fires once per tick)

module alu (
    // Clock and reset — only used by multi-cycle ops (not yet implemented).
    // verilator lint_off UNUSEDSIGNAL
    input  logic        i_clk,
    input  logic        i_rst,
    // verilator lint_on UNUSEDSIGNAL

    // Operands and operation select
    input  logic [31:0] i_a,       // A-bus operand
    input  logic [31:0] i_b,       // B-bus operand (from B-mux)
    input  logic [4:0]  i_op,      // ALU operation select (5-bit, see encoding below)
    input  logic        i_carry_in,// Carry flag from SR (used by ADC/SBC)

    // Multi-cycle control
    // verilator lint_off UNUSEDSIGNAL
    input  logic        i_start,   // Pulse high for 1 cycle to begin a multi-cycle op
                                   // (ignored for single-cycle ops)
    // verilator lint_on UNUSEDSIGNAL
    output logic        o_busy,    // High while a multi-cycle op is in progress
                                   // (always 0 for single-cycle ops)

    // Result and flags
    output logic [31:0] o_result,  // ALU result → R-bus
    output logic        o_flag_z,  // Zero:     result == 0
    output logic        o_flag_n,  // Negative: result[31]
    output logic        o_flag_c,  // Carry:    carry-out (ARM-style: C = NOT borrow on SUB)
    output logic        o_flag_v   // Overflow: signed overflow
);

    // ── Operation codes ─────────────────────────────────────────
    // 5-bit encoding. Single-cycle ops are 00000–01010.
    // Multi-cycle ops are 01011–10000 (initially trapped as illegal
    // instructions via microcode ROM, hardware added later).
    // 10001–11111 reserved for future FP ops.

    // Single-cycle operations
    localparam logic [4:0] OP_ADD    = 5'b00000;
    localparam logic [4:0] OP_SUB    = 5'b00001;
    localparam logic [4:0] OP_AND    = 5'b00010;
    localparam logic [4:0] OP_OR     = 5'b00011;
    localparam logic [4:0] OP_XOR    = 5'b00100;
    localparam logic [4:0] OP_SHL    = 5'b00101;
    localparam logic [4:0] OP_SHR    = 5'b00110;
    localparam logic [4:0] OP_SAR    = 5'b00111;
    localparam logic [4:0] OP_PASS_A = 5'b01000;
    localparam logic [4:0] OP_PASS_B = 5'b01001;
    localparam logic [4:0] OP_NOT    = 5'b01010;
    localparam logic [4:0] OP_ADC    = 5'b01011;  // add with carry
    localparam logic [4:0] OP_SBC    = 5'b01100;  // subtract with borrow

    // Peer-unit (divmul) op encodings. The ALU produces 0 for these; the
    // divmul peer reads the same op field and computes the result.
    // verilator lint_off UNUSEDPARAM
    localparam logic [4:0] OP_MUL    = 5'b01101;  // signed multiply
    localparam logic [4:0] OP_MULU   = 5'b01110;  // unsigned multiply
    localparam logic [4:0] OP_DIV    = 5'b01111;  // signed divide
    localparam logic [4:0] OP_DIVU   = 5'b10000;  // unsigned divide
    // verilator lint_on UNUSEDPARAM
    // 5'b10001–5'b11111: reserved for future ops

    // ── Adder with subtract support ─────────────────────────────
    // SUB is implemented as: A + ~B + 1  (two's complement subtraction)
    //
    // In C terms:  sub_mode ? (a + (~b) + 1) : (a + b + 0)
    //              which equals: sub_mode ? (a - b) : (a + b)
    //
    // The XOR trick: b ^ 0xFFFFFFFF == ~b (flip all bits)
    // Adding carry-in of 1 completes the two's complement: ~b + 1 == -b

    logic        sub_mode;
    logic [31:0] b_eff;       // B after conditional inversion
    logic        cin;         // Carry-in: depends on operation
    logic [32:0] adder_full;  // 33-bit result to capture carry-out
    logic [31:0] adder_result;
    logic        adder_cout;

    // SUB/SBC invert B; ADD/ADC do not.
    assign sub_mode     = (i_op == OP_SUB) || (i_op == OP_SBC);
    assign b_eff        = i_b ^ {32{sub_mode}};  // XOR with all-1s = bitwise NOT
    // Carry-in:
    //   ADD: 0
    //   SUB: 1 (completes two's complement: A + ~B + 1 = A - B)
    //   ADC: carry flag from SR
    //   SBC: carry flag from SR (ARM convention: A + ~B + C = A - B - !C)
    always_comb begin
        case (i_op)
            OP_ADC:  cin = i_carry_in;
            OP_SBC:  cin = i_carry_in;
            default: cin = sub_mode;  // 1 for SUB, 0 for ADD/others
        endcase
    end
    assign adder_full   = {1'b0, i_a} + {1'b0, b_eff} + {32'b0, cin};
    assign adder_result = adder_full[31:0];
    assign adder_cout   = adder_full[32];

    // ── Shift results ───────────────────────────────────────────
    // B[4:0] is the shift amount (0-31), same as C: x << (n & 0x1F)
    logic [4:0]  shamt;
    logic [31:0] shl_result;
    logic [31:0] shr_result;
    logic [31:0] sar_result;
    logic        shl_carry;   // Last bit shifted out (MSB side)
    logic        shr_carry;   // Last bit shifted out (LSB side)

    assign shamt      = i_b[4:0];
    assign shl_result = i_a << shamt;
    assign shr_result = i_a >> shamt;
    // SAR: arithmetic right shift — in C, >> on signed is implementation-defined,
    // but in SystemVerilog, $signed() with >>> explicitly fills with the sign bit.
    assign sar_result = $signed(i_a) >>> shamt;

    // Carry for shifts: the last bit that "fell off" the edge.
    // If shift amount is 0, carry = 0 (nothing shifted out).
    assign shl_carry = (shamt == 0) ? 1'b0 : i_a[32 - shamt];
    assign shr_carry = (shamt == 0) ? 1'b0 : i_a[shamt - 1];

    // ── Core operation select (single-cycle) ────────────────────
    // `always_comb` = "re-evaluate this block whenever any input changes"
    // It's like a C switch statement that the hardware evaluates continuously.
    // `case (i_op)` = multiplexer: select one of N results based on i_op.

    logic [31:0] result_mux;  // Pre-flag result
    logic        carry_mux;   // Carry output (operation-dependent)

    always_comb begin
        case (i_op)
            OP_ADD: begin
                result_mux = adder_result;
                carry_mux = adder_cout;
            end
            OP_SUB: begin
                result_mux = adder_result;
                carry_mux = adder_cout;
            end
            OP_AND: begin
                result_mux = i_a & i_b;
                carry_mux = 1'b0;
            end
            OP_OR: begin
                result_mux = i_a | i_b;
                carry_mux = 1'b0;
            end
            OP_XOR: begin
                result_mux = i_a ^ i_b;
                carry_mux = 1'b0;
            end
            OP_SHL: begin
                result_mux = shl_result;
                carry_mux = shl_carry;
            end
            OP_SHR: begin
                result_mux = shr_result;
                carry_mux = shr_carry;
            end
            OP_SAR: begin
                result_mux = sar_result;
                carry_mux = shr_carry;
            end
            OP_PASS_A: begin
                result_mux = i_a;
                carry_mux = 1'b0;
            end
            OP_PASS_B: begin
                result_mux = i_b;
                carry_mux = 1'b0;
            end
            OP_NOT: begin
                result_mux = ~i_b;
                carry_mux = 1'b0;
            end
            OP_ADC: begin
                result_mux = adder_result;
                carry_mux = adder_cout;
            end
            OP_SBC: begin
                result_mux = adder_result;
                carry_mux = adder_cout;
            end
            default: begin
                result_mux = 32'b0;
                carry_mux = 1'b0;
            end
        endcase
    end

    // ── Multi-cycle result placeholder ──────────────────────────
    // The ALU is single-cycle. Multi-cycle MUL/DIV are computed by the
    // divmul peer unit (divmul.sv), which reads the same op field, drives
    // the multi-cycle busy, and whose result the datapath muxes onto the
    // R-bus. Here the ALU simply forces its own output to 0 (and o_busy
    // low) for the peer-unit op codes.

    logic        multicycle_busy;
    logic [31:0] multicycle_result;

    assign multicycle_busy   = 1'b0;  // Stub: never busy (no multi-cycle HW yet)
    assign multicycle_result = 32'b0; // Stub: no result

    // ── Output mux and flags ────────────────────────────────────
    // Select between single-cycle (combinational) and multi-cycle results.
    logic is_multicycle_op;
    assign is_multicycle_op = (i_op >= OP_MUL);  // MUL (0x0D) and above are multi-cycle

    assign o_result = is_multicycle_op ? multicycle_result : result_mux;
    assign o_busy   = multicycle_busy;

    assign o_flag_z = (o_result == 32'b0);
    assign o_flag_n = o_result[31];
    assign o_flag_c = is_multicycle_op ? 1'b0 : carry_mux;

    // V: signed overflow — only meaningful for ADD/SUB.
    // Both operands same sign, but result differs → overflow.
    // Uses b_eff (B after conditional inversion) so the same logic
    // works for both ADD (b_eff = B) and SUB (b_eff = ~B).
    assign o_flag_v = (i_op == OP_ADD || i_op == OP_SUB ||
                       i_op == OP_ADC || i_op == OP_SBC)
                    ? (i_a[31] == b_eff[31]) && (o_result[31] != i_a[31])
                    : 1'b0;

endmodule
