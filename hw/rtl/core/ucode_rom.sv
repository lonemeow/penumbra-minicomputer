// Penumbra Microcode ROM — 256 × 52-bit, combinational read
//
// Loaded from hex file at synthesis/simulation time via $readmemh.
// On ECP5, this maps to 3 EBRs (Embedded Block RAMs). In discrete,
// this would be 7 byte-wide ROM chips.

module ucode_rom (
    input  logic [7:0]  i_addr,     // Micro-PC address
    output logic [51:0] o_uword     // 52-bit micro-word
);

    logic [51:0] rom [0:255];

    initial begin
        // Default all entries to zero (safe NOP: pc=HOLD, no enables)
        for (int i = 0; i < 256; i++)
            rom[i] = 52'b0;
        $readmemh("microcode.hex", rom);
    end

    assign o_uword = rom[i_addr];

endmodule
