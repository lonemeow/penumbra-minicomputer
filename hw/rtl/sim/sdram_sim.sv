// Sim-only SDRAM subsystem: sdram_stack + sim PHY + chip model.
//
// Bus interface mirrors simple_mem so machine_sim can swap
// in this stack at the RAM region without other plumbing changes.
// Internally instantiates the full v2 SDRAM stack for end-to-end
// verification of the controller against the behavioral model.
//
// As of step 4 the CDC bridge sits between the bus adapter and the
// controller.  The two clocks `i_clk` (system / CPU domain) and
// `i_sdram_clk` (controller / chip domain) are separate ports — the
// testbench drives them at the hardware ratio (e.g. 4× SDRAM cycles
// per CPU cycle to match ULX3S's 25 MHz CPU / 100 MHz SDRAM) so the
// memory latency observed in CPU cycles matches what hardware sees.
// A same-rate setup (`i_sdram_clk` tied to `i_clk`) is still valid
// for unit tests that only care about FSM/handshake correctness;
// metastability behaviour is a hardware concern outside Verilator's
// model regardless.
//
// Uses the W9825-100MHz preset with the small `W9825_SIM_T_POWERUP`
// so init completes in a few cycles instead of 200 µs of sim time.

module sdram_sim
    import sdram_pkg::*;
(
    input  logic        i_clk,         // system / CPU clock
    input  logic        i_sdram_clk,   // SDRAM / controller clock (faster)
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy
);

    // Chip preset.  Sim sites override only T_POWERUP at the stack
    // so unit tests don't wait the full 200 µs JEDEC power-up sequence.
    localparam sdram_params_t SDP = W9825_100;

    // ── Stack ↔ PHY ────────────────────────────────────────
    logic [3:0]                  ctrl_cmd;
    logic                        ctrl_cke;
    logic [SDP.ROW_BITS-1:0]     ctrl_a;
    logic [SDP.BA_BITS-1:0]      ctrl_ba;
    logic [SDP.DQ_BITS/8-1:0]    ctrl_dqm;
    logic [SDP.DQ_BITS-1:0]      ctrl_dq_out;
    logic                        ctrl_dq_oe;
    logic [SDP.DQ_BITS-1:0]      ctrl_dq_in;
    logic                        init_done_unused;

    // The sim PHY is combinational, so the stack keeps its zero
    // PHY-latency defaults.
    sdram_stack #(
        .SDP             (SDP),
        .T_POWERUP       (W9825_SIM_T_POWERUP)
    ) u_stack (
        .i_sys_clk       (i_clk),
        .i_sys_rst       (i_rst),
        .i_sd_clk        (i_sdram_clk),
        .i_sd_rst        (i_rst),
        .i_addr          (i_addr),
        .i_wdata         (i_wdata),
        .i_byte_en       (i_byte_en),
        .i_we            (i_we),
        .i_re            (i_re),
        .o_rdata         (o_rdata),
        .o_busy          (o_busy),
        .o_phy_cmd       (ctrl_cmd),
        .o_phy_cke       (ctrl_cke),
        .o_phy_a         (ctrl_a),
        .o_phy_ba        (ctrl_ba),
        .o_phy_dqm       (ctrl_dqm),
        .o_phy_dq_out    (ctrl_dq_out),
        .o_phy_dq_oe     (ctrl_dq_oe),
        .i_phy_dq_in     (ctrl_dq_in),
        .o_dbg_init_done (init_done_unused)
    );

    // ── PHY → chip pin signals ────────────────────────────
    logic                        sd_clk, sd_cke, sd_csn, sd_rasn, sd_casn, sd_wen;
    logic [SDP.ROW_BITS-1:0]     sd_a;
    logic [SDP.BA_BITS-1:0]      sd_ba;
    logic [SDP.DQ_BITS/8-1:0]    sd_dqm;
    wire  [SDP.DQ_BITS-1:0]      sd_d;

    sdram_phy_sim #(
        .ROW_BITS (SDP.ROW_BITS),
        .BA_BITS  (SDP.BA_BITS),
        .DQ_BITS  (SDP.DQ_BITS)
    ) u_phy (
        .i_clk        (i_sdram_clk),
        .i_phy_cmd    (ctrl_cmd),
        .i_phy_cke    (ctrl_cke),
        .i_phy_a      (ctrl_a),
        .i_phy_ba     (ctrl_ba),
        .i_phy_dqm    (ctrl_dqm),
        .i_phy_dq_out (ctrl_dq_out),
        .i_phy_dq_oe  (ctrl_dq_oe),
        .o_phy_dq_in  (ctrl_dq_in),
        .o_sdram_clk  (sd_clk),
        .o_sdram_cke  (sd_cke),
        .o_sdram_csn  (sd_csn),
        .o_sdram_rasn (sd_rasn),
        .o_sdram_casn (sd_casn),
        .o_sdram_wen  (sd_wen),
        .o_sdram_a    (sd_a),
        .o_sdram_ba   (sd_ba),
        .o_sdram_dqm  (sd_dqm),
        .io_sdram_d   (sd_d)
    );

    sdram_model #(
        .ROW_BITS (SDP.ROW_BITS),
        .COL_BITS (SDP.COL_BITS),
        .BA_BITS  (SDP.BA_BITS),
        .DQ_BITS  (SDP.DQ_BITS)
    ) u_model (
        .i_clk  (sd_clk),
        .i_cke  (sd_cke),
        .i_csn  (sd_csn),
        .i_rasn (sd_rasn),
        .i_casn (sd_casn),
        .i_wen  (sd_wen),
        .i_a    (sd_a),
        .i_ba   (sd_ba),
        .i_dqm  (sd_dqm),
        .io_d   (sd_d)
    );

    // ══════════════════════════════════════════════════════════
    // Read-after-write consistency checker (sim-only, always on)
    //
    // The one invariant the whole memory stack must hold: a read
    // returns the value most recently written to that address.  This
    // shadows every committed write (per byte) and checks every read's
    // o_rdata against it, firing on the FIRST violation.  It is the net
    // that catches stale/transposed reads from the speculative-prefetch
    // path — the class of bug that otherwise only surfaces as a far-
    // downstream hang.  It runs under every sim that instantiates this
    // module (gen2 conformance, benchmarks, interactive), so a
    // regression is reported at the cycle it happens, with the address
    // and the expected/got words, instead of being chased by hand.
    //
    // Windowed to the low region (where vectors, handlers, kernel and
    // benchmark code/data/stack live) to keep the shadow light; the
    // per-byte written mask means never-written bytes are not compared.
    // The transaction address is captured while o_busy is high so the
    // adapter's PRESENT->BEGIN address change cannot misattribute a
    // completion.
    // ══════════════════════════════════════════════════════════
    localparam int CHK_WORDS = 512*1024;             // low 2 MiB

    logic [31:0] chk_shadow [CHK_WORDS];
    logic [3:0]  chk_wmask  [CHK_WORDS];
    logic [31:0] cap_addr, cap_wdata;
    logic [3:0]  cap_byte_en;
    logic        cap_we, cap_re, busy_q;

    wire         cap_inrange = (cap_addr < (CHK_WORDS*4));
    wire [18:0]  cap_word    = cap_addr[20:2];
    wire         cap_done    = busy_q && !o_busy;     // busy 1->0 = this txn completed

    // Free-running cycle counter — only to locate a violation in the log.
    logic [31:0] cyc_count;

    integer chk_i;
    initial begin
        for (chk_i = 0; chk_i < CHK_WORDS; chk_i = chk_i + 1) chk_wmask[chk_i] = 4'b0;
        cyc_count = 32'd0;
    end

    always_ff @(posedge i_clk) begin
        cyc_count <= cyc_count + 1;
        busy_q    <= o_busy;
        if (o_busy) begin
            cap_addr    <= i_addr;
            cap_wdata   <= i_wdata;
            cap_byte_en <= i_byte_en;
            cap_we      <= i_we;
            cap_re      <= i_re;
        end

        if (!i_rst && cap_done && cap_inrange) begin
            if (cap_we) begin
                if (cap_byte_en[0]) chk_shadow[cap_word][ 7: 0] <= cap_wdata[ 7: 0];
                if (cap_byte_en[1]) chk_shadow[cap_word][15: 8] <= cap_wdata[15: 8];
                if (cap_byte_en[2]) chk_shadow[cap_word][23:16] <= cap_wdata[23:16];
                if (cap_byte_en[3]) chk_shadow[cap_word][31:24] <= cap_wdata[31:24];
                chk_wmask[cap_word] <= chk_wmask[cap_word] | cap_byte_en;
            end else if (cap_re) begin
                for (int b = 0; b < 4; b = b + 1)
                    if (chk_wmask[cap_word][b] &&
                        (o_rdata[b*8 +: 8] !== chk_shadow[cap_word][b*8 +: 8])) begin
                        $display("=== SDRAM STALE READ @0x%08x exp 0x%08x got 0x%08x (byte %0d, cyc %0d) ===",
                                 cap_addr, chk_shadow[cap_word], o_rdata, b, cyc_count);
                        $fatal(1, "sdram_sim: stale read");
                    end
            end
        end
    end

endmodule
