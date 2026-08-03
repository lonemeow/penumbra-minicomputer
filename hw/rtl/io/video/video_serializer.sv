// Penumbra video 10:1 DDR serializer — one TMDS lane.
//
// Takes the encoder's 10-bit word once per pixel clock and emits it
// LSB-first as five DDR bit-pairs per word on the 5x serial clock:
// o_d0 carries the serial-clock-high half-bit and o_d1 the low half,
// matching ODDRX1F's D0/D1 sampling, so bit 2n rides o_d0 and bit
// 2n+1 rides o_d1. The board top owns the ODDRX1F and the pad; this
// module is pure logic, so one source serves Verilator and synthesis
// (the sdram_ctrl / sdram_phy_ecp5 split, one level down). The DVI
// clock lane needs no dedicated hardware: a fourth instance fed the
// constant word 10'b0000011111 emits the pixel clock through the same
// launch path as the data lanes.
//
// Clock contract: i_sclk is exactly 5x i_pclk from the same PLL —
// frequency-locked, arbitrary but fixed phase. Cross-domain nets are
// therefore timed related-clock paths, not asynchronous crossings: a
// single sampling register is synchronization enough, and there is no
// metastability window to absorb. The pixel domain re-registers the
// incoming word and flips a boundary marker; the serial domain finds
// word boundaries by watching that marker, never by trusting reset
// release order across the two domains.
//
// Stream contract (pinned by tb_video_serializer): once locked, every
// word appears exactly once, LSB-first, five pairs back-to-back with
// no gaps or slips, at a constant latency; lock is (re)established
// within five words of reset release.

module video_serializer (
    input  logic       i_pclk,   // pixel clock — word rate
    input  logic       i_sclk,   // serial clock — 5x pixel, same PLL
    input  logic       i_rst,    // synchronous, active-high, spans both domains
    input  logic [9:0] i_word,   // TMDS word, valid at each pixel clock
    output logic       o_d0,     // DDR pair, serial-clock-high bit (ODDRX1F D0)
    output logic       o_d1      // DDR pair, serial-clock-low bit (ODDRX1F D1)
);

    // ── Pixel domain: word capture and boundary marker ──────────
    // word_q gives the serial domain a full pixel period to take the
    // word from; ptoggle_q flips exactly once per word, so the serial
    // domain can locate pixel-period boundaries without sharing the
    // clock that made them.
    logic [9:0] word_q;
    logic       ptoggle_q;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            word_q    <= '0;
            ptoggle_q <= 1'b0;
        end else begin
            word_q    <= i_word;
            ptoggle_q <= ~ptoggle_q;
        end
    end

    // ── Serial domain: boundary detect, capture, shift ──────────
    // shift_q consumes two bits per serial cycle from the bottom;
    // load_word restarts it from word_q on a word boundary.
    // The boundary marker's two most recent serial-domain samples —
    // [0] this cycle's, [1] the previous cycle's. A word boundary is
    // exactly a difference between the taps; the capture then lands
    // one cycle into word_q's five-cycle stability window.
    logic [1:0] ptoggle_hist_q;
    logic [9:0] shift_q;
    logic       load_word;   // capture word_q into shift_q this cycle

    assign load_word = ptoggle_hist_q[0] != ptoggle_hist_q[1];

    always_ff @(posedge i_sclk) begin
        if (i_rst) begin
            ptoggle_hist_q <= 2'b00;
            shift_q        <= '0;
        end else begin
            ptoggle_hist_q <= {ptoggle_hist_q[0], ptoggle_q};
            shift_q        <= load_word ? word_q : (shift_q >> 2);
        end
    end

    assign o_d0 = shift_q[0];
    assign o_d1 = shift_q[1];

`ifdef VERILATOR
    // Capture cadence: after the first load, loads arrive exactly
    // every five serial cycles — anything else is a word slip on the
    // wire, which a monitor shows as a rolling or torn image.
    logic [2:0] since_load_q;
    logic       seen_load_q;
    always_ff @(posedge i_sclk) begin
        if (i_rst) begin
            since_load_q <= '0;
            seen_load_q  <= 1'b0;
        end else begin
            since_load_q <= load_word ? 3'd1
                          : (since_load_q == 3'd7 ? 3'd7 : since_load_q + 3'd1);
            seen_load_q  <= seen_load_q | load_word;
        end
    end
    load_cadence: assert property (@(posedge i_sclk) disable iff (i_rst)
        (!load_word || !seen_load_q || since_load_q == 3'd5))
        else $error("word capture slipped: %0d cycles since the last", since_load_q);
`endif

endmodule
