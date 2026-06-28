// penumbra3_translate -- gen3 address translation for one port (launch/resolve).
//
// One translation port: the gen3 MMU instantiates it twice -- an instruction
// (I) copy and a data (D) copy -- over a shared install/refill write stream, so
// the two copies stay hardware-coherent. Nothing here is side-specific; the
// access type and privilege are inputs.
//
// Launch cycle: derive the set index from the effective address and launch the
// main TLB copy read. Resolve cycle: the registered set feeds the verdict cone,
// which combines with the pinned-TLB result to produce the translation. The
// query (vaddr, access type, mode, ASID) is registered launch->resolve to align
// with the BRAM read latency. The pinned-TLB lookup is async and supplied at the
// resolve cycle (pinned-hit-wins).
module penumbra3_translate
    import penumbra_pkg::*;
#(
    parameter int SETS = 32,
    parameter int WAYS = 2
) (
    input  logic                    i_clk,
    input  logic                    i_rst,

    // MEM1 query (registered out of EX)
    input  logic                    i_lookup_en,
    input  logic [31:0]             i_vaddr,
    input  logic [2:0]              i_access_type,
    input  logic                    i_user_mode,
    input  logic [7:0]              i_asid,
    input  logic                    i_hold,   // freeze MEM1->MEM2 query + the TLB read (pipe stall)

    // Pinned-TLB result for this access (async, presented at MEM2)
    input  logic                    i_pinned_hit,
    input  logic [31:0]             i_pinned_pte_word,

    // MEM2 verdict
    output logic [31:0]             o_paddr,
    output logic                    o_cacheable,
    output logic                    o_hit,
    output logic                    o_miss_fault,
    output logic                    o_prot_fault,

    // Install / refill / invalidate (to the D-copy store)
    input  logic                    i_wr_en,
    input  logic [$clog2(SETS)-1:0] i_wr_set,
    input  logic [$clog2(WAYS)-1:0] i_wr_way,
    input  logic                    i_wr_valid,
    input  logic [31:0]             i_wr_vpn_word,
    input  logic [31:0]             i_wr_pte_word
);

    localparam int SET_BITS = $clog2(SETS);

    // VPN[19:0] = vaddr[31:12]; set index = low VPN bits; page offset [11:0].
    logic [SET_BITS-1:0] rd_set;
    assign rd_set = i_vaddr[12 +: SET_BITS];

    // ── MEM1 -> MEM2 query registers ─────────────────────────────
    logic        lookup_en_q;
    logic [19:0] vpn_q;
    logic [2:0]  acc_type_q;
    logic        user_mode_q;
    logic [7:0]  asid_q;
    logic [11:0] page_off_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            lookup_en_q <= 1'b0;
            vpn_q       <= '0;
            acc_type_q  <= '0;
            user_mode_q <= 1'b0;
            asid_q      <= '0;
            page_off_q  <= '0;
        end else if (!i_hold) begin
            lookup_en_q <= i_lookup_en;
            vpn_q       <= i_vaddr[31:12];
            acc_type_q  <= i_access_type;
            user_mode_q <= i_user_mode;
            asid_q      <= i_asid;
            page_off_q  <= i_vaddr[11:0];
        end
    end

    // ── Main-TLB D-copy: MEM1 launch / MEM2 registered output ────
    logic [WAYS-1:0]       rd_valid;
    logic [WAYS-1:0][31:0] rd_vpn_word;
    logic [WAYS-1:0][31:0] rd_pte_word;
    penumbra3_tlb_store #(
        .SETS (SETS),
        .WAYS (WAYS)
    ) u_store (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_rd_set      (rd_set),
        .i_hold        (i_hold),
        .o_rd_valid    (rd_valid),
        .o_rd_vpn_word (rd_vpn_word),
        .o_rd_pte_word (rd_pte_word),
        .i_wr_en       (i_wr_en),
        .i_wr_set      (i_wr_set),
        .i_wr_way      (i_wr_way),
        .i_wr_valid    (i_wr_valid),
        .i_wr_vpn_word (i_wr_vpn_word),
        .i_wr_pte_word (i_wr_pte_word)
    );

    // ── MEM2 verdict cone ────────────────────────────────────────
    penumbra3_tlb_verdict #(
        .WAYS (WAYS)
    ) u_verdict (
        .i_lookup_en       (lookup_en_q),
        .i_vpn             (vpn_q),
        .i_asid            (asid_q),
        .i_access_type     (acc_type_q),
        .i_user_mode       (user_mode_q),
        .i_page_off        (page_off_q),
        .i_valid           (rd_valid),
        .i_vpn_word        (rd_vpn_word),
        .i_pte_word        (rd_pte_word),
        .i_pinned_hit      (i_pinned_hit),
        .i_pinned_pte_word (i_pinned_pte_word),
        .o_paddr           (o_paddr),
        .o_cacheable       (o_cacheable),
        .o_hit             (o_hit),
        .o_prot_fault      (o_prot_fault)
    );

    assign o_miss_fault = lookup_en_q && !o_hit;

endmodule
