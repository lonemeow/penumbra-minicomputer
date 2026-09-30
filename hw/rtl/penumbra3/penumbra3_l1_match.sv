// penumbra3_l1_match -- the gen3 L1 hit verdict (resolve cycle).
//
// Pure combinational. Given one set's ways, registered out of
// penumbra3_l1_store, and the frame candidates the MMU resolves beside them,
// it produces the per-way hit and the hit way's word.
//
// The compare is parallel, not serial. Waiting for the translated address
// would put the main-TLB match, its way select and the pinned/bypass
// override in front of the tag compare. Instead every cache way's tag is
// compared against every candidate frame at once -- the early one the
// bypass or pinned TLB supplies and each main-TLB way's -- and the MMU's
// one-hot frame select picks among the results, so the depth is the slower
// of the two compares plus an AND-OR rather than their sum. The same shape
// serves IF2 and MEM2; each side's way count sets how wide it is.
//
// A line is never resident in two ways (a fill installs only on a miss), so
// the way hit is one-hot and the word is an AND-OR over the ways.
module penumbra3_l1_match #(
    parameter  int WAYS   = 2,
    parameter  int FRAMES = 3,     // candidate frames: the early one + each TLB way
    localparam int TAG_BITS = 20
) (
    input  logic [WAYS-1:0]               i_valid,
    input  logic [WAYS-1:0][TAG_BITS-1:0] i_tag,
    input  logic [WAYS-1:0][31:0]         i_word,

    input  logic [FRAMES-1:0][19:0]       i_frame,
    input  logic [FRAMES-1:0]             i_frame_sel,   // one-hot; empty on a TLB miss

    output logic [WAYS-1:0]               o_way_hit,     // one-hot
    output logic                          o_hit,
    output logic [31:0]                   o_word
);

    always_comb begin
        for (int w = 0; w < WAYS; w++) begin
            o_way_hit[w] = 1'b0;
            for (int k = 0; k < FRAMES; k++)
                o_way_hit[w] |= i_frame_sel[k] && (i_tag[w] == i_frame[k]);
            o_way_hit[w] &= i_valid[w];
        end
    end

    assign o_hit = |o_way_hit;

    always_comb begin
        o_word = '0;
        for (int w = 0; w < WAYS; w++)
            o_word |= {32{o_way_hit[w]}} & i_word[w];
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    always_comb
        assert ($onehot0(i_frame_sel))
            else $error("penumbra3_l1_match: frame select not one-hot");

    always_comb
        assert ($onehot0(o_way_hit))
            else $error("penumbra3_l1_match: a line is resident in two ways");

endmodule
