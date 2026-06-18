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

    logic [8:0] q_m;
    always_comb begin
        q_m[0] = i_data[0];
        for (int i = 1; i < 8; i++)
            q_m[i] = use_xnor ? (q_m[i-1] ~^ i_data[i])
                              : (q_m[i-1]  ^ i_data[i]);
        q_m[8] = ~use_xnor;
    end

    // 1s / 0s among the eight data bits of q_m (q_m[8] is the flag bit,
    // not counted). Stage 2 steers the line using their difference.
    logic [3:0] qm_ones, qm_zeros;
    assign qm_ones = {3'b0, q_m[0]} + {3'b0, q_m[1]}
                   + {3'b0, q_m[2]} + {3'b0, q_m[3]}
                   + {3'b0, q_m[4]} + {3'b0, q_m[5]}
                   + {3'b0, q_m[6]} + {3'b0, q_m[7]};
    assign qm_zeros = 4'd8 - qm_ones;

    // ── Stage 2: DC balancing ──────────────────────────────────
    // Running disparity (signed): positive => more 1s than 0s have been
    // transmitted so far. From q_m, the 1/0 counts above, and this
    // accumulator, stage 2 picks the final 10-bit word (optionally
    // inverting the low 8 bits, marked by data_word[9]) and the
    // disparity it leaves behind. Registered below, so the value read
    // here is "disparity before this pixel".
    logic signed [5:0] disp;

    logic [9:0]        data_word;   // active-video word for this pixel
    logic signed [5:0] disp_next;   // running disparity after data_word

    always_comb begin
        if (disp == 0 || qm_ones == qm_zeros) begin
            data_word = 
        end
        // TODO(human): DVI 1.0 stage-2 DC balancing — drive data_word
        // and disp_next from q_m, qm_ones/qm_zeros, and disp.
        data_word = 10'b0;
        disp_next = disp;
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
            disp   <= '0;
        end else if (i_de) begin
            o_tmds <= data_word;
            disp   <= disp_next;
        end else begin
            o_tmds <= ctrl_word;
            disp   <= '0;               // control periods recenter disparity
        end
    end

`ifdef VERILATOR
    // The DVI running disparity is provably small; a larger magnitude
    // means the stage-2 update arithmetic is wrong.
    disp_bound: assert property (@(posedge i_clk) disable iff (i_rst)
        (disp >= -6'sd16 && disp <= 6'sd16))
        else $error("TMDS disparity out of bounds: %0d", disp);
`endif

endmodule
