// Bus Device Select — address comparator for device-side decode
//
// Combinational address decoder: asserts o_sel when i_addr falls
// within [BASE, BASE+SIZE). SIZE must be a power of 2 and BASE
// must be naturally aligned — both enforced at elaboration time.
//
// Each instantiation corresponds to one device's address comparator
// on the shared bus. In discrete 74xx, this is a 74x85 magnitude
// comparator on the upper address lines, with SIZE determining how
// many low bits are "don't care" (like DIP switches on an ISA card).
//
// Usage:
//   bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_WORDS * 4)))
//       u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));

module bus_devsel #(
    parameter logic [31:0] BASE = 32'h0,
    parameter logic [31:0] SIZE = 32'h0
) (
    input  logic [31:0] i_addr,
    output logic        o_sel
);

    // Elaboration-time checks — catch misconfiguration before
    // simulation or synthesis produces silent misdecode.
    build_check #(
        .COND (SIZE != 0),
        .MSG  ("bus_devsel: SIZE must be non-zero")
    ) u_check_nonzero ();

    build_check #(
        .COND ((SIZE & (SIZE - 1)) == 0),
        .MSG  ("bus_devsel: SIZE must be a power of 2")
    ) u_check_pow2 ();

    build_check #(
        .COND ((BASE & (SIZE - 1)) == 0),
        .MSG  ("bus_devsel: BASE must be naturally aligned to SIZE")
    ) u_check_aligned ();

    assign o_sel = (i_addr & ~(SIZE - 1)) == BASE;

endmodule
