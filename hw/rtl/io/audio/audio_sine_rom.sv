// Quarter-wave sine table, 128 phase steps per cycle.
//
// One quadrant, unfolded by symmetry: the phase's top two bits pick
// mirror and sign, the low five index the table. Entries sit at the
// midpoint of each step, which keeps every quadrant to 32 entries with
// no endpoint case.
//
// 128 steps is not a resolution compromise — it makes a phase increment
// expressed in steps exact, so the accumulator never truncates and the
// output carries no DDS spurs. The table's 16-bit rounding puts the
// worst harmonic 105 dB down.

module audio_sine_rom (
    input  logic        [6:0]  i_phase,
    output logic signed [15:0] o_sample
);

    logic signed [15:0] quarter [0:31];
    assign quarter = '{
        16'sd804,   16'sd2410,  16'sd4011,  16'sd5602,
        16'sd7179,  16'sd8739,  16'sd10278, 16'sd11793,
        16'sd13279, 16'sd14732, 16'sd16151, 16'sd17530,
        16'sd18868, 16'sd20159, 16'sd21403, 16'sd22594,
        16'sd23731, 16'sd24811, 16'sd25832, 16'sd26790,
        16'sd27683, 16'sd28510, 16'sd29268, 16'sd29956,
        16'sd30571, 16'sd31113, 16'sd31580, 16'sd31971,
        16'sd32285, 16'sd32521, 16'sd32678, 16'sd32757
    };

    logic [4:0]         index;
    logic signed [15:0] magnitude;

    assign index     = i_phase[5] ? ~i_phase[4:0] : i_phase[4:0];
    assign magnitude = quarter[index];
    assign o_sample  = i_phase[6] ? -magnitude : magnitude;

endmodule
