// Penumbra Ethernet frame check sequence generator
//
// Computes the 32-bit CRC that ends an Ethernet frame, one byte per cycle.
// Like the USB CRCs, the bare module emits only the running remainder in
// o_crc and leaves the framing to its user: the transmitter sends the
// remainder's complement as the check sequence, and the receiver runs the
// remainder on through the check sequence it was sent and compares what is
// left with a constant.
//
// The remainder is held reflected, the coefficient of x^31 in bit 0.
// Ethernet sends each byte least significant bit first and the check
// sequence from its x^31 coefficient down, so in this form the check
// sequence goes out as the remainder's four bytes, low byte first, and no
// bit of it is reversed on the way.
//
// This CRC sits on a byte-streaming path, so it folds a whole byte per
// cycle: the serial Galois step applied eight times.
//
// Generator polynomial G(x) = x^32 + x^26 + x^23 + x^22 + x^16 + x^12 +
// x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1. The remainder is
// seeded to all-ones at the start of each frame.

module eth_crc32 (
    input  logic        i_clk,
    input  logic        i_init,   // reseed the remainder for a new frame
    input  logic        i_valid,  // i_data carries a frame byte this cycle
    input  logic [7:0]  i_data,   // next frame byte (LSB transmitted first)
    output logic [31:0] o_crc     // running remainder, reflected
);
    localparam logic [31:0] CRC32_SEED = 32'hFFFF_FFFF;

    // Tap mask for G(x), reflected as the remainder is: bit k is the
    // coefficient of x^(31-k). x^32 is the bit shifted out of the register
    // and folded back as feedback.
    localparam logic [31:0] CRC32_POLY = 32'hEDB8_8320;

    logic [31:0] crc_q;   // running remainder (registered)
    logic [31:0] crc_d;   // next remainder (combinational)

    assign o_crc = crc_q;

    // One byte folded through the Galois LFSR, a bit at a time in the order
    // the wire carries them, data[0] first. The loop is combinational;
    // synthesis unrolls it into a fixed XOR cone.
    function automatic logic [31:0] crc32_byte(input logic [31:0] crc_in,
                                               input logic [7:0]  data);
        logic [31:0] crc;       // the remainder as the bits go in
        logic        feedback;  // whether this bit's step applies the taps
        crc = crc_in;
        for (int i = 0; i < 8; i++) begin
            feedback = crc[0] ^ data[i];
            crc = (crc >> 1) ^ (feedback ? CRC32_POLY : '0);
        end
        crc32_byte = crc;
    endfunction

    always_comb begin
        if (i_init)
            crc_d = CRC32_SEED;
        else if (i_valid)
            crc_d = crc32_byte(crc_q, i_data);
        else
            crc_d = crc_q;
    end

    // No reset: the remainder is data, and i_init seeds it before every
    // frame.
    always_ff @(posedge i_clk) begin
        crc_q <= crc_d;
    end
endmodule
