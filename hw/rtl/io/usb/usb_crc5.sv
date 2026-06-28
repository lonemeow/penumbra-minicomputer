// Penumbra USB token CRC5 generator
//
// Computes the 5-bit token CRC defined by USB 2.0 over the 11-bit
// {ENDPOINT, DEVADDR} field of an IN / OUT / SETUP token. The MAC feeds
// the token field one bit per cycle; the running remainder in o_crc is
// what the MAC complements and bit-reverses to form the on-wire CRC5.
//
// The token CRC sits off the byte-streaming data path (one short field
// per token, not per payload byte), so a bit-serial shift form is the
// natural shape here; the payload CRC16 is where a byte-parallel form
// earns its keep.
//
// Generator polynomial G(x) = x^5 + x^2 + 1 (tap mask 0x05). The
// remainder is seeded to all-ones at the start of each token and
// advanced on every valid bit.

module usb_crc5 (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic       i_init,   // reseed the remainder for a new token
    input  logic       i_valid,  // i_bit carries a token-field bit this cycle
    input  logic       i_bit,    // next token-field bit
    output logic [4:0] o_crc     // running remainder
);
    // USB seeds the CRC remainder to all-ones before the first bit, so a
    // leading run of zeros still changes the result.
    localparam logic [4:0] CRC5_SEED = 5'h1F;

    // Tap mask for G(x) = x^5 + x^2 + 1 (bits 2 and 0); x^5 is the bit
    // shifted out of the register and folded back as feedback.
    localparam logic [4:0] CRC5_POLY = 5'h05;

    logic [4:0] crc_q;   // running remainder (registered)
    logic [4:0] crc_d;   // next remainder (combinational)

    assign o_crc = crc_q;

    logic feedback;
    assign feedback = crc_q[4] ^ i_bit;

    always_comb begin
        if (i_init)
            crc_d = CRC5_SEED;
        else if (i_valid)
            // Galois LFSR step: shift the remainder up one, then XOR the
            // taps back in when the shifted-out bit fed back.
            crc_d = {crc_q[3:0], 1'b0} ^ (feedback ? CRC5_POLY : 5'h00);
        else
            crc_d = crc_q;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            crc_q <= CRC5_SEED;
        else
            crc_q <= crc_d;
    end
endmodule
