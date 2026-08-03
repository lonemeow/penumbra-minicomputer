// Penumbra video TMDS line encoder — one DVI 1.0 8b/10b channel.
//
// Turns one 8-bit color byte into the 10-bit word a GPDI pair carries.
// During active video (i_de) it transition-minimizes the byte (stage 1)
// and then DC-balances it against the running disparity (stage 2), so
// the AC-coupled serial line stays edge-sparse and centered around zero
// DC. During blanking (!i_de) it emits one of four fixed control codes
// selected by {i_c1, i_c0}: on the blue channel those carry hsync/vsync,
// on red/green the control bits are tied 0. A control period also
// recenters the running disparity.
//
// The disparity accumulator counts in halved units — one count per
// excess *pair* of 1s — which keeps every constant and comparison one
// bit narrower than spec bit-units. Units are an encoder-internal
// choice: the receiver never sees the accumulator, only the words,
// and those are bit-exact with the DVI 1.0 algorithm.
//
// One pixel-clock of latency: stage 1, stage 2, and the control-code
// select are combinational; the 10-bit word and the disparity
// accumulator are registered. PHY-independent — the ODDR serializer
// downstream shifts o_tmds out LSB-first at 10x the pixel rate. This is
// the first FPGA-only block past the parallel-RGB seam, but it is plain
// synchronous logic and so is fully verifiable in simulation against an
// independent software model before any clocking or ODDR exists.

module video_tmds_encoder (
    input  logic       i_clk,    // pixel clock
    input  logic       i_rst,    // synchronous, active-high
    input  logic [7:0] i_data,   // color byte, valid while i_de
    input  logic       i_c0,     // control bit 0 (blue: hsync), used while !i_de
    input  logic       i_c1,     // control bit 1 (blue: vsync), used while !i_de
    input  logic       i_de,     // data enable — active video when high
    output logic [9:0] o_tmds    // encoded word, serialized LSB-first
);

    // ── Stage 1: transition minimization ───────────────────────
    // Chain the input bits with XOR or XNOR so the intermediate word
    // q_m carries the fewest 0<->1 transitions. The choice is made from
    // the input's 1s-count (a byte with mostly 1s, or exactly four 1s
    // with bit 0 clear, minimizes better under XNOR). q_m[8] records the
    // choice (1 = XOR, 0 = XNOR) so the receiver can undo the chain.
    logic [3:0] data_ones;
    assign data_ones = {3'b0, i_data[0]} + {3'b0, i_data[1]}
                     + {3'b0, i_data[2]} + {3'b0, i_data[3]}
                     + {3'b0, i_data[4]} + {3'b0, i_data[5]}
                     + {3'b0, i_data[6]} + {3'b0, i_data[7]};

    logic use_xnor;
    assign use_xnor = (data_ones > 4'd4) ||
                      (data_ones == 4'd4 && i_data[0] == 1'b0);

    // Only the XOR prefix chain is computed: the XNOR chain equals it
    // with every odd-indexed bit inverted (each stage's ~(x ^ 1) = x
    // cancels the previous inversion), so choosing between the chains
    // is a constant-mask XOR on the finished parities, not a mux inside
    // the chain. Written as independent prefix parities — no bit
    // depends on another, and synthesis builds each as its own tree.
    logic [7:0] parity;
    assign parity = { ^i_data[7:0], ^i_data[6:0], ^i_data[5:0], ^i_data[4:0],
                      ^i_data[3:0], ^i_data[2:0], ^i_data[1:0], i_data[0] };

    logic [8:0] q_m;
    assign q_m = {~use_xnor, parity ^ (use_xnor ? 8'hAA : 8'h00)};

    // Excess 1s among the eight data bits of q_m (q_m[8] is the flag
    // bit, not counted), in halved units: +4 for all 1s, -4 for all 0s,
    // 0 for a balanced byte. Stage 2 steers the line with this.
    logic [3:0] qm_ones;
    assign qm_ones = {3'b0, q_m[0]} + {3'b0, q_m[1]}
                   + {3'b0, q_m[2]} + {3'b0, q_m[3]}
                   + {3'b0, q_m[4]} + {3'b0, q_m[5]}
                   + {3'b0, q_m[6]} + {3'b0, q_m[7]};

    logic signed [4:0] qm_excess;
    assign qm_excess = $signed({1'b0, qm_ones}) - 5'sd4;

    // ── Stage 2: DC balancing ──────────────────────────────────
    // Running disparity (halved units, signed): positive => more 1s
    // than 0s have left the line so far. From q_m, qm_excess, and this
    // accumulator, stage 2 picks the final 10-bit word — data_word[9]
    // set marks the low 8 bits inverted, data_word[8] carries q_m[8]
    // through for the receiver — and the disparity the word leaves
    // behind. Registered below, so disp_q reads as "disparity before
    // this pixel".
    //
    // Invariant: whatever word goes out, disp_d must absorb its actual
    // excess — disp_d == disp_q + (ones(data_word) - 5). The
    // track_word assertion below holds every branch to that.
    logic signed [4:0] disp_q;

    logic [9:0]        data_word;   // active-video word for this pixel
    logic signed [4:0] disp_d;      // running disparity after data_word

    always_comb begin
        if (disp_q == 0 || qm_excess == 0) begin
            // Line centered, or a balanced symbol that cannot steer it:
            // emit q_m plain under XOR (q_m[8] set), fully inverted
            // under XNOR, so bit 9 still marks the inversion.
            data_word = {~q_m[8], q_m[8], q_m[8] ? q_m[7:0] : ~q_m[7:0]};
            disp_d    = q_m[8] ? disp_q + qm_excess : disp_q - qm_excess;
        end else begin
            if (qm_excess[4] == disp_q[4]) begin
                data_word = {1'b1, q_m[8], ~q_m[7:0]};
                disp_d    = disp_q - qm_excess + (q_m[8] ? 1 : 0);
            end else begin
                data_word = {1'b0, q_m[8], q_m[7:0]};
                disp_d    = disp_q + qm_excess - (q_m[8] ? 0 : 1);
            end
        end
    end

    // ── Control-period codes ────────────────────────────────────
    // The four fixed words sent during blanking, indexed by {c1, c0}.
    // Deliberately high-transition so a receiver can character-align to
    // them; they also reset the running disparity to zero.
    logic [9:0] ctrl_word;
    always_comb begin
        unique case ({i_c1, i_c0})
            2'b00:   ctrl_word = 10'b1101010100;
            2'b01:   ctrl_word = 10'b0010101011;
            2'b10:   ctrl_word = 10'b0101010100;
            default: ctrl_word = 10'b1010101011;
        endcase
    end

    // ── Output / disparity registers ────────────────────────────
    // Active video emits the balanced data word and carries the
    // disparity forward; blanking emits a control code and recenters.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_tmds <= 10'b1101010100;   // {vsync,hsync}=0 control code
            disp_q <= '0;
        end else if (i_de) begin
            o_tmds <= data_word;
            disp_q <= disp_d;
        end else begin
            o_tmds <= ctrl_word;
            disp_q <= '0;               // control periods recenter disparity
        end
    end

`ifdef VERILATOR
    // The disparity register mirrors the line's true running excess:
    // every emitted word must be absorbed exactly, or the encoder's
    // idea of the line drifts from the line itself.
    logic signed [4:0] word_excess;
    always_comb begin
        word_excess = -5'sd5;
        for (int i = 0; i < 10; i++)
            if (data_word[i]) word_excess = word_excess + 5'sd1;
    end
    track_word: assert property (@(posedge i_clk) disable iff (i_rst)
        (!i_de || disp_d == disp_q + word_excess))
        else $error("stage-2 disparity update does not absorb the emitted word");

    // The DVI steering keeps the running disparity provably small; a
    // larger magnitude means the stage-2 decision or update is wrong.
    disp_bound: assert property (@(posedge i_clk) disable iff (i_rst)
        (disp_q >= -5'sd8 && disp_q <= 5'sd8))
        else $error("TMDS disparity out of bounds: %0d", disp_q);
`endif

endmodule
