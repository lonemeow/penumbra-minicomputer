// ULX3S board top (Penumbra/3 probe P0.3) -- MEM1/MEM2 + BRAM-TLB timing.
//
// A synthesis instrument, not a usable machine. It puts the gen3 data-side
// translate cone in front of nextpnr: penumbra3_translate (the D-copy BRAM
// TLB read launched in MEM1, the verdict resolved in MEM2) plus a second
// store standing in for the I-copy, so the duplicated BRAM usage and the
// shared write fan-out are realistic. The path under measurement is the
// registered BRAM output -> way match -> way mux -> pinned override ->
// permission verdict -> output flop, the cone whose depth decides whether
// gen3 earns the BRAM TLB at 50 MHz.
//
// An LFSR drives the query every cycle and the buttons perturb it, so the
// cone cannot constant-fold; every translate output and the I-copy read
// fold onto the LEDs so synthesis cannot prune the design. Clocking is the
// 25 MHz crystal directly (no PLL), one clean domain for comparable fmax.
//
// Probe simplification: the pinned-TLB result is modeled as a registered
// input (pinned_hit_q / pinned_pte_q), so the pinned-override *mux* is on
// the measured cone but tlb_pinned's own async compare is not -- that part
// is confirmed in the full-core build, not here.
module ulx3s_penumbra3_probe_memtlb_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en      // LOW = hold ESP32 in reset
);
    import penumbra_pkg::*;

    localparam int SETS = 32;
    localparam int WAYS = 2;
    localparam int SET_BITS = $clog2(SETS);
    localparam int WAY_BITS = $clog2(WAYS);

    assign wifi_en = 1'b0;

    // ── Reset: power-on counter + btn[1] (FIRE1) ─────────────────
    logic btn1_sync1_q, btn1_sync2_q;
    always_ff @(posedge clk_25mhz) begin
        btn1_sync1_q <= btn[1];
        btn1_sync2_q <= btn1_sync1_q;
    end

    /* verilator lint_off PROCASSINIT */
    logic [18:0] rst_cnt_q = '0;
    /* verilator lint_on PROCASSINIT */
    logic rst;
    always_ff @(posedge clk_25mhz) begin
        if (btn1_sync2_q)      rst_cnt_q <= '0;
        else if (!rst_cnt_q[18]) rst_cnt_q <= rst_cnt_q + 1;
    end
    assign rst = !rst_cnt_q[18];

    // ── Stimulus: a 32-bit maximal LFSR, perturbed by the buttons ─
    logic [31:0] lfsr_q;
    always_ff @(posedge clk_25mhz) begin
        if (rst)
            lfsr_q <= 32'h1;
        else
            lfsr_q <= {lfsr_q[30:0],
                       lfsr_q[31] ^ lfsr_q[21] ^ lfsr_q[1] ^ lfsr_q[0] ^ btn[2]};
    end

    // Access type must be one-hot (the verdict asserts it).
    logic [2:0] acc_type;
    always_comb begin
        case (lfsr_q[1:0])
            2'd0:    acc_type = ACC_READ;
            2'd1:    acc_type = ACC_WRITE;
            default: acc_type = ACC_EXEC;
        endcase
    end

    // Pinned-TLB model: registered so it presents at MEM2 like the real
    // (async) pinned lookup would.
    logic        pinned_hit_q;
    logic [31:0] pinned_pte_q;
    always_ff @(posedge clk_25mhz) begin
        pinned_hit_q <= lfsr_q[9] & lfsr_q[10];
        pinned_pte_q <= {lfsr_q[15:0], lfsr_q[31:16]};
    end

    // Shared install/refill/invalidate stream (fans to both copies).
    logic                wr_en;
    logic [SET_BITS-1:0] wr_set;
    logic [WAY_BITS-1:0] wr_way;
    logic                wr_valid;
    logic [31:0]         wr_vpn_word;
    logic [31:0]         wr_pte_word;
    assign wr_en       = lfsr_q[3] & lfsr_q[4];
    assign wr_set      = lfsr_q[SET_BITS-1:0];
    assign wr_way      = lfsr_q[8];
    assign wr_valid    = lfsr_q[11];
    assign wr_vpn_word = lfsr_q;
    assign wr_pte_word = {lfsr_q[7:0], lfsr_q[31:8]};

    // ── DUT: the gen3 data-side translate cone (D-copy inside) ───
    logic [31:0] paddr;
    logic        cacheable, hit, miss_fault, prot_fault;
    /* verilator lint_off PINCONNECTEMPTY */
    penumbra3_translate #(
        .SETS (SETS),
        .WAYS (WAYS)
    ) u_dtranslate (
        .i_clk             (clk_25mhz),
        .i_rst             (rst),
        .i_lookup_en       (lfsr_q[7]),
        .i_vaddr           (lfsr_q),
        .i_access_type     (acc_type),
        .i_user_mode       (lfsr_q[5]),
        .i_asid            (lfsr_q[23:16]),
        .i_hold            (1'b0),
        .i_pinned_hit      (pinned_hit_q),
        .i_pinned_pte_word (pinned_pte_q),
        .o_paddr           (paddr),
        .o_cacheable       (cacheable),
        .o_hit             (hit),
        .o_miss_fault      (miss_fault),
        .o_prot_fault      (prot_fault),
        .o_rd_vpn_word     (),
        .o_rd_pte_word     (),
        .i_wr_en           (wr_en),
        .i_wr_set          (wr_set),
        .i_wr_way          (wr_way),
        .i_wr_valid        (wr_valid),
        .i_wr_vpn_word     (wr_vpn_word),
        .i_wr_pte_word     (wr_pte_word)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // ── I-copy stand-in: real BRAM usage + shared write fan-out ──
    logic [WAYS-1:0]       icopy_valid;
    logic [WAYS-1:0][31:0] icopy_vpn_word;
    logic [WAYS-1:0][31:0] icopy_pte_word;
    penumbra3_tlb_store #(
        .SETS (SETS),
        .WAYS (WAYS)
    ) u_icopy (
        .i_clk         (clk_25mhz),
        .i_rst         (rst),
        .i_rd_set      (lfsr_q[SET_BITS+3:4]),
        .i_hold        (1'b0),
        .o_rd_valid    (icopy_valid),
        .o_rd_vpn_word (icopy_vpn_word),
        .o_rd_pte_word (icopy_pte_word),
        .i_wr_en       (wr_en),
        .i_wr_set      (wr_set),
        .i_wr_way      (wr_way),
        .i_wr_valid    (wr_valid),
        .i_wr_vpn_word (wr_vpn_word),
        .i_wr_pte_word (wr_pte_word)
    );

    // ── Verdict register: the real MEM2 -> consumer flop ────────
    // The measured cone must end here, at a clean register, exactly as the
    // verdict feeds the load-completion / WB flop in the core. Capturing the
    // verdict before the LED fold keeps the keep-alive XOR tree off the
    // cone -- folding the combinational verdict straight to the LEDs would
    // put a 32-bit XOR on the path that the real design never has.
    logic [31:0] paddr_q;
    logic        cacheable_q, hit_q, miss_fault_q, prot_fault_q;
    always_ff @(posedge clk_25mhz) begin
        paddr_q      <= paddr;
        cacheable_q  <= cacheable;
        hit_q        <= hit;
        miss_fault_q <= miss_fault;
        prot_fault_q <= prot_fault;
    end

    // ── Keep-alive: fold the registered outputs onto the LEDs ────
    // Every term here is a flop output, so this XOR is a separate off-cone
    // path, not part of the verdict timing.
    logic [7:0] led_q;
    always_ff @(posedge clk_25mhz) begin
        led_q <= {7'b0,
                  (^paddr_q)
                  ^ cacheable_q ^ hit_q ^ miss_fault_q ^ prot_fault_q
                  ^ (|icopy_valid)
                  ^ (^icopy_vpn_word[0]) ^ (^icopy_pte_word[0])
                  ^ (^icopy_vpn_word[1]) ^ (^icopy_pte_word[1])};
    end
    assign led = led_q;

endmodule
