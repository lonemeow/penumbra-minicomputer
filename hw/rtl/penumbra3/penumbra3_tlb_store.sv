// penumbra3_tlb_store -- one copy of the gen3 main TLB (sync-read BRAM).
//
// 2-way set-associative storage for one translation port. The set index is
// driven in MEM1 and the indexed set (both ways) is registered out in MEM2,
// so the verdict cone downstream starts from flops -- the split that earns
// back a BRAM TLB (see doc/internals/penumbra3/mem-stage.md). VPN/PTE words
// live in BRAM; the valid bits live in a resettable flop array, because BRAM
// contents are indeterminate out of reset and the TLB must read as empty
// until the miss handler fills it.
//
// Entry layout matches the sysreg TLB interface:
//   vpn_word = {4'b0, VPN[19:0], ASID[7:0]}
//   pte_word = {PPN[19:0], SW[7:0], flags}
// One write port serves miss refill and sysreg install; the gen3 MMU drives
// it on both the I- and D-copies in the same cycle to keep them coherent.
//
// A write suppresses that cycle's read, so the RTL itself says the two
// ports never meet at one address and synthesis adds no read-during-write
// bypass after the EBR. Nothing is lost: every write is a WRSYS, which
// commits only once the back end has drained and then resyncs the front
// end, so neither copy has a live lookup launching in that cycle.
module penumbra3_tlb_store #(
    parameter int SETS = 32,
    parameter int WAYS = 2
) (
    input  logic                    i_clk,
    input  logic                    i_rst,

    // MEM1: launch the set read
    input  logic [$clog2(SETS)-1:0] i_rd_set,
    input  logic                    i_hold,   // freeze the registered read (pipe stall = read CE low)

    // MEM2: registered set contents, per way
    output logic [WAYS-1:0]         o_rd_valid,
    output logic [WAYS-1:0][31:0]   o_rd_vpn_word,
    output logic [WAYS-1:0][31:0]   o_rd_pte_word,

    // Install / refill / invalidate (one way of one set)
    input  logic                    i_wr_en,
    input  logic [$clog2(SETS)-1:0] i_wr_set,
    input  logic [$clog2(WAYS)-1:0] i_wr_way,
    input  logic                    i_wr_valid,
    input  logic [31:0]             i_wr_vpn_word,
    input  logic [31:0]             i_wr_pte_word
);

    // Registered read output, held on a pipe stall so a frozen MEM2 slot
    // keeps its own set (the read clock-enable; the write port is never
    // gated, so an install in flight is not dropped).
    logic rd_en;
    assign rd_en = !i_hold && !i_wr_en;

    // VPN/PTE words: BRAM (sync read, registered output lands in MEM2), one
    // memory per way so both synthesis tools build each as a block RAM. The
    // block attribute forces EBR so the probe measures the real Tco floor.
    for (genvar gw = 0; gw < WAYS; gw++) begin : g_way
        (* ram_style = "block" *) logic [31:0] vpn_mem [SETS];
        (* ram_style = "block" *) logic [31:0] pte_mem [SETS];
        logic [31:0] vpn_q, pte_q;

        logic way_we;
        assign way_we = i_wr_en && (i_wr_way == $clog2(WAYS)'(gw));

        always_ff @(posedge i_clk) begin
            if (rd_en) begin
                vpn_q <= vpn_mem[i_rd_set];
                pte_q <= pte_mem[i_rd_set];
            end
            if (way_we) begin
                vpn_mem[i_wr_set] <= i_wr_vpn_word;
                pte_mem[i_wr_set] <= i_wr_pte_word;
            end
        end
        assign o_rd_vpn_word[gw] = vpn_q;
        assign o_rd_pte_word[gw] = pte_q;
    end

    // Valid bits: resettable flops (BRAM is indeterminate out of reset).
    logic [WAYS-1:0][SETS-1:0] valid_q;
    always_ff @(posedge i_clk) begin
        if (rd_en)
            for (int w = 0; w < WAYS; w++)
                o_rd_valid[w] <= valid_q[w][i_rd_set];

        if (i_rst)
            valid_q <= '0;
        else if (i_wr_en)
            valid_q[i_wr_way][i_wr_set] <= i_wr_valid;
    end

endmodule
