// Penumbra Byte Extractor — sub-word extraction and sign extension
//
// Extracts a byte or halfword from a 32-bit memory word based on
// the byte offset (addr[1:0]) and access size (mem_size). Optionally
// sign-extends the result. For word accesses, passes through unchanged.
//
// Purely combinational — a mux tree with optional sign fill.
//
// mem_size encoding (matches microcode):
//   00 = byte (8-bit)
//   01 = halfword (16-bit)
//   10 = word (32-bit, pass-through)

module byte_ext (
    input  logic [31:0] i_data,      // Full 32-bit word from memory
    input  logic [1:0]  i_addr_lo,   // Byte offset (addr[1:0])
    input  logic [1:0]  i_size,      // Access size: 00=byte, 01=half, 10=word
    input  logic        i_sign_ext,  // 1=sign-extend, 0=zero-extend

    output logic [31:0] o_data       // Extracted and extended result
);

    logic [7:0]  byte_sel;
    logic [15:0] half_sel;

    // ── Byte selection (4:1 mux) ─────────────────────────────
    always_comb begin
        case (i_addr_lo)
            2'b00: byte_sel = i_data[ 7: 0];
            2'b01: byte_sel = i_data[15: 8];
            2'b10: byte_sel = i_data[23:16];
            2'b11: byte_sel = i_data[31:24];
        endcase
    end

    // ── Halfword selection (2:1 mux) ─────────────────────────
    always_comb begin
        case (i_addr_lo[1])
            1'b0: half_sel = i_data[15: 0];
            1'b1: half_sel = i_data[31:16];
        endcase
    end

    // ── Size mux with sign/zero extension ────────────────────
    always_comb begin
        case (i_size)
            2'b00:   // Byte
                o_data = i_sign_ext ? {{24{byte_sel[7]}}, byte_sel}
                                    : {24'b0, byte_sel};
            2'b01:   // Halfword
                o_data = i_sign_ext ? {{16{half_sel[15]}}, half_sel}
                                    : {16'b0, half_sel};
            default: // Word (pass-through)
                o_data = i_data;
        endcase
    end

endmodule
