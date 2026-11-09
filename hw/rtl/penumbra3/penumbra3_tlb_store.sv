// penumbra3_tlb_store -- one copy of the gen3 main TLB (sync-read BRAM).
//
// Set-associative storage for one translation port. The set index is
// driven in MEM1 and the indexed set (every way) is registered out in MEM2,
// so the verdict cone downstream starts from flops -- the split that earns
// back a BRAM TLB (see doc/internals/penumbra3/mem-stage.md).
//
// Entry layout matches the sysreg TLB interface:
//   vpn_word = {4'b0, VPN[19:0], ASID[7:0]}
//   pte_word = {PPN[19:0], SW[7:0], flags}
// An entry's valid bit is its PTE word's V, stored with it: a flop array
// beside the words would put a SETS-way read mux and a SETS-way write
// decode on every copy, where the word read is already paid for. Block RAM
// comes out of reset holding anything, which the architecture allows: the
// TLB is undefined after reset, and software invalidates it before it
// turns translation on (doc/system/mmu.md, "Boot Sequence").
//
// One write port serves install and invalidation. It writes one set: the
// VPN word to the ways its enable names, and each way's own PTE word to the
// ways its enable names, so one write can clear V in several ways of a set.
// The gen3 MMU drives it on both the I- and D-copies in the same cycle to
// keep them coherent.
//
// A write suppresses that cycle's read, so the RTL itself says the two
// ports never meet at one address and synthesis adds no read-during-write
// bypass after the EBR. Nothing is lost: every write comes from a WRSYS,
// which commits only once the back end has drained and then resyncs the
// front end, or from the MMU's invalidation sweep, which holds fetch while
// it runs, so neither copy has a live lookup launching in that cycle.
module penumbra3_tlb_store #(
    parameter int SETS = 32,
    parameter int WAYS = 2
) (
    input  logic                    i_clk,

    // MEM1: launch the set read
    input  logic [$clog2(SETS)-1:0] i_rd_set,
    input  logic                    i_hold,   // freeze the registered read (pipe stall = read CE low)

    // MEM2: registered set contents, per way
    output logic [WAYS-1:0][31:0]   o_rd_vpn_word,
    output logic [WAYS-1:0][31:0]   o_rd_pte_word,

    // Write one set: install, invalidate
    input  logic [$clog2(SETS)-1:0] i_wr_set,
    input  logic [WAYS-1:0]         i_wr_vpn_en,
    input  logic [31:0]             i_wr_vpn_word,
    input  logic [WAYS-1:0]         i_wr_pte_en,
    input  logic [WAYS-1:0][31:0]   i_wr_pte_word
);

    // Registered read output, held on a pipe stall so a frozen MEM2 slot
    // keeps its own set (the read clock-enable; the write port is never
    // gated, so a write in flight is not dropped).
    logic rd_en;
    assign rd_en = !i_hold && !(|i_wr_vpn_en) && !(|i_wr_pte_en);

    // VPN/PTE words: BRAM (sync read, registered output lands in MEM2), one
    // memory per way so both synthesis tools build each as a block RAM. The
    // block attribute forces EBR so the probe measures the real Tco floor.
    for (genvar gw = 0; gw < WAYS; gw++) begin : g_way
        (* ram_style = "block" *) logic [31:0] vpn_mem [SETS];
        (* ram_style = "block" *) logic [31:0] pte_mem [SETS];
        logic [31:0] vpn_q, pte_q;

        always_ff @(posedge i_clk) begin
            if (rd_en) begin
                vpn_q <= vpn_mem[i_rd_set];
                pte_q <= pte_mem[i_rd_set];
            end
            if (i_wr_vpn_en[gw])
                vpn_mem[i_wr_set] <= i_wr_vpn_word;
            if (i_wr_pte_en[gw])
                pte_mem[i_wr_set] <= i_wr_pte_word[gw];
        end
        assign o_rd_vpn_word[gw] = vpn_q;
        assign o_rd_pte_word[gw] = pte_q;
    end

endmodule
