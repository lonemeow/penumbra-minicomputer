// Penumbra video test-pattern generator.
//
// A pure function of the active pixel coordinate: given (i_x, i_y) and
// the timing generator's i_de, it produces the 24-bit RGB shown on
// screen. This is the power-up / "uninitialized" image source — a fixed
// pattern that proves the whole display pipeline (timing -> RGB -> TMDS
// -> monitor) is alive before any cell memory, font, or CPU exists. It
// later becomes one selectable source behind the pixel mux, alongside
// the character-cell fetch.
//
// Combinational and PHY-independent, so it sits on the parallel-RGB
// seam and is verified by rendering a frame to an image in simulation.

module video_pattern_gen #(
    // Active resolution; defaults track console mode 0 (see video_pkg).
    parameter int H_ACTIVE = video_pkg::H_ACTIVE,
    parameter int V_ACTIVE = video_pkg::V_ACTIVE
) (
    // Coordinates carry headroom above the active resolution, so a given
    // pattern reads only the low bits; the upper bits are intentionally
    // unused.
    // verilator lint_off UNUSEDSIGNAL
    input  logic [11:0] i_x,    // active column, valid while i_de
    input  logic [11:0] i_y,    // active row,    valid while i_de
    // verilator lint_on UNUSEDSIGNAL
    input  logic        i_de,   // data enable from the timing generator
    output logic [7:0]  o_r,
    output logic [7:0]  o_g,
    output logic [7:0]  o_b
);

    // Active-region edge coordinates, sized to the 12-bit raster
    // position so the comparisons are width-matched.
    localparam logic [11:0] X_LAST = 12'(H_ACTIVE - 1);
    localparam logic [11:0] Y_LAST = 12'(V_ACTIVE - 1);

    localparam int H_BITS = $clog2(H_ACTIVE);

    // Active-region pattern color. Blanking is forced black below, so
    // this only needs to define the visible image.
    logic [7:0] r, g, b;

    // Test card: a 1-pixel white border around two halves. The top half
    // is vertical bars — red, green, blue, white, then black — each a
    // Y-ramp, isolating the R/G/B channels and the all-on / all-off
    // extremes. The bottom half is an 8x8 grayscale checkerboard, also
    // Y-ramped, which stresses pixel-rate addressing and serialization.
    // Ramps run bright-at-top via bitwise NOT (~i_y), which inverts an
    // 8-bit value with inverters only — no adder.

    logic [2:0] bar_sel;
    assign bar_sel = i_x[H_BITS-1 : H_BITS-3];

    always_comb begin
        if (i_x == 12'd0 || i_x == X_LAST ||
            i_y == 12'd0 || i_y == Y_LAST) begin
            r = 8'hFF; g = 8'hFF; b = 8'hFF;   // white border
        end else if (!i_y[8]) begin
            // Top part: Y-ramp RGB + gray + black bars

            r = 8'b0;
            g = 8'b0;
            b = 8'b0;

            case (bar_sel)
                3'd0: r = ~i_y[7:0];
                3'd1: g = ~i_y[7:0];
                3'd2: b = ~i_y[7:0];
                3'd3: begin
                    r = ~i_y[7:0];                    
                    g = ~i_y[7:0];                    
                    b = ~i_y[7:0];                    
                end
                default: ;   // other bars stay black (pre-assigned above)
            endcase
        end else begin
            // Bottom part: Y-ramp grayscale checkerboard
            if (i_y[3]) begin
                r = i_x[3] ? ~i_y[7:0] : 8'b0;
                g = i_x[3] ? ~i_y[7:0] : 8'b0;
                b = i_x[3] ? ~i_y[7:0] : 8'b0;
            end else begin
                r = i_x[3] ? 8'b0 : ~i_y[7:0];
                g = i_x[3] ? 8'b0 : ~i_y[7:0];
                b = i_x[3] ? 8'b0 : ~i_y[7:0];
            end
        end
    end

    // DVI/TMDS carries no pixel data during blanking: force black there.
    assign o_r = i_de ? r : 8'd0;
    assign o_g = i_de ? g : 8'd0;
    assign o_b = i_de ? b : 8'd0;

endmodule
