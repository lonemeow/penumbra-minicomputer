// Penumbra USB data CRC16 generator
//
// Computes the 16-bit CRC defined by USB 2.0 over a data packet payload,
// one byte per cycle. Like the token CRC5, the bare module emits only the
// running remainder in o_crc; the 1's-complement and bit-reverse that form
// the on-wire CRC16 are the MAC's job, applied above this module.
//
// This CRC sits on the byte-streaming data path, so it folds a whole byte
// per cycle — the serial Galois step applied eight times — to hold the MAC's
// one-byte-per-cycle datapath instead of stalling eight cycles per byte.
//
// Generator polynomial G(x) = x^16 + x^15 + x^2 + 1 (tap mask 0x8005). The
// remainder is seeded to all-ones at the start of each packet.

module usb_crc16 (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic        i_init,   // reseed the remainder for a new packet
    input  logic        i_valid,  // i_data carries a payload byte this cycle
    input  logic [7:0]  i_data,   // next payload byte (LSB transmitted first)
    output logic [15:0] o_crc     // running remainder
);
    localparam logic [15:0] CRC16_SEED = 16'hFFFF;

    // Tap mask for G(x) = x^16 + x^15 + x^2 + 1 (bits 15, 2, and 0); x^16 is
    // the bit shifted out of the register and folded back as feedback.
    localparam logic [15:0] CRC16_POLY = 16'h8005;

    logic [15:0] crc_q;   // running remainder (registered)
    logic [15:0] crc_d;   // next remainder (combinational)

    assign o_crc = crc_q;

    // One byte folded through the Galois LFSR: the serial step — feedback from
    // the top bit, shift up one, conditionally XOR the taps — applied to each
    // of the byte's eight bits, LSB first to match USB transmission order. The
    // loop is combinational; synthesis unrolls it into a fixed XOR cone.
    function automatic logic [15:0] crc16_byte(input logic [15:0] crc_in,
                                               input logic [7:0]  data);
        logic [15:0] crc;
        logic        feedback;
        crc = crc_in;
        for (int i = 0; i < 8; i++) begin
            // The Galois step, once per bit: feedback from the top bit, shift
            // up one, conditionally XOR the taps. Eight passes, LSB first.
            feedback = crc[15] ^ data[i];
            crc = (crc << 1) ^ (feedback ? CRC16_POLY : '0);
        end
        crc16_byte = crc;
    endfunction

    always_comb begin
        if (i_init)
            crc_d = CRC16_SEED;
        else if (i_valid)
            crc_d = crc16_byte(crc_q, i_data);
        else
            crc_d = crc_q;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            crc_q <= CRC16_SEED;
        else
            crc_q <= crc_d;
    end
endmodule
