// SDR SDRAM controller core — generic, single-domain, single-word interface.
//
// Drives a per-FPGA-family PHY (sdram_phy_ecp5 in hardware,
// sdram_phy_sim in Verilator).  Parameterized over chip geometry and
// timing; presets in sdram_pkg.sv.
//
// Row management: single-row tracking.  After each RD/WR the row
// stays open (auto-precharge bit cleared in the command).  A new
// request that hits the open bank+row skips ACTIVATE entirely; a
// row miss issues an explicit PRECHARGE-ALL before the new ACTIVATE.
// Refreshes always close any open row first.  See
// doc/internals/sdram-optimization.md § Level 1.
//
// See doc/internals/sdram-controller.md for the design plan.

// keep_hierarchy: prevent yosys from flattening this module into the
// parent. Combined with the same attribute on sdram_cdc, this stops
// ABC from sharing LUT4s across the controller / CDC boundary, which
// keeps nextpnr placing controller cells in a single cluster.
// Without this, the placer scattered controller logic across the
// chip and the open_valid → o_phy_a.CE control path failed 100 MHz
// timing (8.7 ns of routing across 10 hops vs. 2.1 ns of actual
// logic — verified via make timing BOARD=ulx3s CORE=penumbra1 before this change).
(* keep_hierarchy = "yes" *)
module sdram_ctrl
    import sdram_pkg::*;
#(
    // ── Geometry ────────────────────────────────────────
    parameter int ROW_BITS  = 13,
    parameter int COL_BITS  = 9,
    parameter int BA_BITS   = 2,
    parameter int DQ_BITS   = 16,

    // ── Timings (cycles) ─────────────────────────────────
    parameter int T_RCD       = 2,
    parameter int T_RP        = 2,
    parameter int T_RFC       = 7,
    parameter int T_WR        = 2,
    parameter int T_MRD       = 2,
    parameter int T_REFI      = 750,
    parameter int T_POWERUP   = 20000,
    parameter int CAS_LATENCY = 2,

    // ── PHY round-trip latency ──────────────────────────
    // Extra cycles introduced by an IOB-registering PHY:
    //   PHY_OUT_LATENCY = output IOB flop(s) between the controller's
    //                     o_phy_* register and the SDRAM pin.
    //   PHY_IN_LATENCY  = input  IOB flop(s) between the SDRAM pin
    //                     and i_phy_dq_in observed by the controller.
    // Both are 0 for the combinational sim PHY, 1 for the ECP5 PHY.
    // The controller uses them to extend the read-data sample
    // countdown (cl_cnt) so beat 0 is captured on the cycle the
    // SDRAM-driven data finishes propagating through the input flop.
    parameter int PHY_OUT_LATENCY = 0,
    parameter int PHY_IN_LATENCY  = 0
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Single-word request/response ──────────────────────
    input  logic                  i_req_valid,
    input  logic                  i_req_we,
    input  logic [31:0]           i_req_addr,
    input  logic [31:0]           i_req_wdata,
    input  logic [3:0]            i_req_byte_en,
    output logic                  o_req_ready,

    output logic                  o_rsp_valid,
    output logic [31:0]           o_rsp_data,
    input  logic                  i_rsp_ready,        // unused; consumer always ready

    // Pulses one cycle when a transaction commits.  For reads this
    // coincides with o_rsp_valid; for writes it pulses on the cycle
    // beat 1 of the write was committed (controller about to enter
    // S_RECOVER).  Lets the bus adapter drop o_busy as early as the
    // cache can use the result.
    output logic                  o_done,

    // ── PHY interface (synchronous to i_clk) ─────────────
    output logic [3:0]            o_phy_cmd,          // {csn, rasn, casn, wen}
    output logic                  o_phy_cke,
    output logic [ROW_BITS-1:0]   o_phy_a,
    output logic [BA_BITS-1:0]    o_phy_ba,
    output logic [DQ_BITS/8-1:0]  o_phy_dqm,
    output logic [DQ_BITS-1:0]    o_phy_dq_out,
    output logic                  o_phy_dq_oe,
    input  logic [DQ_BITS-1:0]    i_phy_dq_in,

    // ── Debug ────────────────────────────────────────────
    output logic                  o_dbg_init_done
);

    // ── Unused inputs (kept for protocol completeness / future use) ──
    // i_rsp_ready: today the consumer is always ready (single-word
    //   handshake, no backpressure).  Kept so the CDC bridge in step 4
    //   can use it as a real handshake.
    // i_req_addr[31:high]: bits above the chip's address space.
    // i_req_addr[0]: byte-in-half-word, handled by i_req_byte_en + DQM.
    // req_col[0]: latched but not consumed — S_ACT forces col[0]=0 for
    //   BL=2 burst alignment regardless of the original byte offset.
    /* verilator lint_off UNUSEDSIGNAL */
    wire _unused = &{1'b0, i_rsp_ready,
                     i_req_addr[31:COL_BITS+ROW_BITS+BA_BITS+1],
                     i_req_addr[0],
                     req_col[0]};
    /* verilator lint_on UNUSEDSIGNAL */

    // ── Mode register value ─────────────────────────────────
    // [2:0]  BL  = 001   (BL=2)
    // [3]    BT  = 0     (sequential)
    // [6:4]  CL        (parameter)
    // [12:7] reserved   (zero)
    localparam logic [ROW_BITS-1:0] MODE_REG =
        {{(ROW_BITS-7){1'b0}}, 3'(CAS_LATENCY), 1'b0, 3'b001};

    // ── FSM states ──────────────────────────────────────────
    typedef enum logic [3:0] {
        S_INIT_WAIT,             // T_POWERUP power-up delay
        S_INIT_PRE,              // PRECHARGE ALL
        S_INIT_REF1,             // first AUTO_REFRESH
        S_INIT_REF2,             // second AUTO_REFRESH
        S_INIT_MRS,              // MODE REGISTER SET
        S_IDLE,
        S_REFRESH,
        S_ACT,                   // wait T_RCD after ACTIVATE
        S_RW,                    // RD/WR command issued; first beat
        S_BURST,                 // (reserved for BL>2 in step 6)
        S_RECOVER,               // wait T_WR before next command
        S_PRECHARGE_TO_ACT,      // row-conflict: PRECHARGE then ACT pending req
        S_PRECHARGE_TO_REFRESH   // pending-refresh with open row: close then refresh
    } state_t;

    state_t state;

    // ── Counters ────────────────────────────────────────────
    localparam int CNT_BITS  = $clog2(T_POWERUP + 1) + 1;
    localparam int REFI_BITS = $clog2(T_REFI + 1) + 1;

    logic [CNT_BITS-1:0]   wait_cnt;
    logic [REFI_BITS-1:0]  refresh_cnt;
    logic                  refresh_pending;

    // ── Latched request ─────────────────────────────────────
    logic                  req_we_r;
    logic [31:0]           req_wdata_r;
    logic [3:0]            req_byte_en_r;
    logic [BA_BITS-1:0]    req_bank;
    logic [ROW_BITS-1:0]   req_row;
    logic [COL_BITS-1:0]   req_col;

    // ── Open-row state (single-row tracking) ────────────────
    // After each RD/WR the bank's row stays open.  A subsequent
    // request that hits open_bank+open_row skips ACTIVATE; any
    // mismatch triggers an explicit PRECHARGE-ALL.  Refreshes also
    // require an open row to close first.
    logic                  open_valid;
    logic [BA_BITS-1:0]    open_bank;
    logic [ROW_BITS-1:0]   open_row;
    wire                   row_hit =
        open_valid && (addr_bank == open_bank) && (addr_row == open_row);

    // ── Burst / read assembly state ─────────────────────────
    logic                  beat;       // 0 = first beat, 1 = second
    logic [3:0]            cl_cnt;     // CAS latency countdown
    logic [DQ_BITS-1:0]    rd_low;
    logic [31:0]           rdata_reg;
    logic                  rsp_valid_r;
    logic                  done_r;

    // ─────────────────────────────────────────────────────────
    // Address mapping
    // ─────────────────────────────────────────────────────────
    // CPU byte address → SDRAM {bank, row, col, byte} layout:
    //
    //   addr[0]                                       byte-in-half-word (DQM)
    //   addr[COL_BITS         : 1]                    column (COL_BITS bits)
    //   addr[COL_BITS+ROW_BITS : COL_BITS+1]          row    (ROW_BITS bits)
    //   addr[COL_BITS+ROW_BITS+BA_BITS :
    //        COL_BITS+ROW_BITS+1]                     bank   (BA_BITS bits)
    //
    // Bank-high keeps consecutive cache lines (16 B) within the same
    // bank+row, favoring the future open-row optimization (step 6:
    // same-row hits skip ACTIVATE).  An interleaved (bank-low) layout
    // would distribute cache lines across banks for multi-outstanding
    // pipelining.
    localparam COL_ADDR_LO_BIT  = 1;
    localparam COL_ADDR_HI_BIT  = COL_BITS;
    localparam ROW_ADDR_LO_BIT  = COL_ADDR_HI_BIT + 1;
    localparam ROW_ADDR_HI_BIT  = ROW_ADDR_LO_BIT + ROW_BITS - 1;
    localparam BANK_ADDR_LO_BIT = ROW_ADDR_HI_BIT + 1;
    localparam BANK_ADDR_HI_BIT = BANK_ADDR_LO_BIT + BA_BITS - 1;
    wire [BA_BITS-1:0]  addr_bank;
    wire [ROW_BITS-1:0] addr_row;
    wire [COL_BITS-1:0] addr_col;
    assign addr_bank = i_req_addr[BANK_ADDR_HI_BIT:BANK_ADDR_LO_BIT];
    assign addr_row  = i_req_addr[ROW_ADDR_HI_BIT:ROW_ADDR_LO_BIT];
    assign addr_col  = i_req_addr[COL_ADDR_HI_BIT:COL_ADDR_LO_BIT];

    // ── Response output ─────────────────────────────────────
    assign o_rsp_valid = rsp_valid_r;
    assign o_rsp_data  = rdata_reg;
    assign o_done      = done_r;
    assign o_phy_cke   = 1'b1;

    // ─────────────────────────────────────────────────────────
    // Refresh timer
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            refresh_cnt     <= '0;
            refresh_pending <= 1'b0;
        end else begin
            if (state == S_INIT_WAIT || state == S_INIT_PRE  ||
                state == S_INIT_REF1 || state == S_INIT_REF2 ||
                state == S_INIT_MRS) begin
                refresh_cnt     <= '0;
                refresh_pending <= 1'b0;
            end else begin
                if (refresh_cnt >= REFI_BITS'(T_REFI)) begin
                    refresh_cnt     <= '0;
                    refresh_pending <= 1'b1;
                end else begin
                    refresh_cnt <= refresh_cnt + 1'b1;
                end
                // Cleared once the FSM dispatches the refresh
                if (state == S_IDLE && refresh_pending && wait_cnt == 0) begin
                    refresh_pending <= 1'b0;
                end
            end
        end
    end

    // ─────────────────────────────────────────────────────────
    // Main FSM
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state            <= S_INIT_WAIT;
            o_phy_cmd        <= SDRAM_CMD_INHIBIT;
            o_phy_a          <= '0;
            o_phy_ba         <= '0;
            o_phy_dqm        <= '1;
            o_phy_dq_out     <= '0;
            o_phy_dq_oe      <= 1'b0;
            o_req_ready      <= 1'b0;
            wait_cnt         <= CNT_BITS'(T_POWERUP);
            beat             <= 1'b0;
            cl_cnt           <= '0;
            rd_low           <= '0;
            rdata_reg        <= 32'hDEAD_BEEF;
            rsp_valid_r      <= 1'b0;
            done_r           <= 1'b0;
            req_we_r         <= 1'b0;
            req_wdata_r      <= '0;
            req_byte_en_r    <= '0;
            req_bank         <= '0;
            req_row          <= '0;
            req_col          <= '0;
            open_valid       <= 1'b0;
            open_bank        <= '0;
            open_row         <= '0;
            o_dbg_init_done  <= 1'b0;
        end else begin
            // ── Defaults ────────────────────────────────────
            o_phy_cmd    <= SDRAM_CMD_NOP;
            o_phy_dqm    <= '1;
            o_phy_dq_oe  <= 1'b0;
            o_req_ready  <= 1'b0;
            rsp_valid_r  <= 1'b0;
            done_r       <= 1'b0;

            case (state)

                // ── Init: power-up wait ─────────────────────
                S_INIT_WAIT: begin
                    o_phy_cmd <= SDRAM_CMD_INHIBIT;
                    if (wait_cnt == 0) begin
                        state <= S_INIT_PRE;
                    end else begin
                        wait_cnt <= wait_cnt - 1'b1;
                    end
                end

                S_INIT_PRE: begin
                    o_phy_cmd   <= SDRAM_CMD_PRECHARGE;
                    o_phy_a     <= '0;
                    o_phy_a[10] <= 1'b1;          // PRECHARGE ALL
                    wait_cnt    <= CNT_BITS'(T_RP);
                    state       <= S_INIT_REF1;
                end

                S_INIT_REF1: begin
                    if (wait_cnt == 0) begin
                        o_phy_cmd <= SDRAM_CMD_REFRESH;
                        wait_cnt  <= CNT_BITS'(T_RFC);
                        state     <= S_INIT_REF2;
                    end else begin
                        wait_cnt  <= wait_cnt - 1'b1;
                    end
                end

                S_INIT_REF2: begin
                    if (wait_cnt == 0) begin
                        o_phy_cmd <= SDRAM_CMD_REFRESH;
                        wait_cnt  <= CNT_BITS'(T_RFC);
                        state     <= S_INIT_MRS;
                    end else begin
                        wait_cnt  <= wait_cnt - 1'b1;
                    end
                end

                S_INIT_MRS: begin
                    if (wait_cnt == 0) begin
                        o_phy_cmd <= SDRAM_CMD_MODE_SET;
                        o_phy_a   <= MODE_REG;
                        o_phy_ba  <= '0;
                        wait_cnt  <= CNT_BITS'(T_MRD);
                        state     <= S_IDLE;
                    end else begin
                        wait_cnt  <= wait_cnt - 1'b1;
                    end
                end

                // ── Steady state ────────────────────────────
                S_IDLE: begin
                    o_dbg_init_done <= 1'b1;
                    if (wait_cnt != 0) begin
                        wait_cnt <= wait_cnt - 1'b1;
                    end else if (refresh_pending) begin
                        // All banks must be idle for AUTO REFRESH.
                        // Close any open row first.
                        if (open_valid) begin
                            o_phy_cmd   <= SDRAM_CMD_PRECHARGE;
                            o_phy_a     <= '0;
                            o_phy_a[10] <= 1'b1;     // PRECHARGE ALL
                            wait_cnt    <= CNT_BITS'(T_RP - 1);
                            open_valid  <= 1'b0;
                            state       <= S_PRECHARGE_TO_REFRESH;
                        end else begin
                            o_phy_cmd <= SDRAM_CMD_REFRESH;
                            wait_cnt  <= CNT_BITS'(T_RFC);
                            state     <= S_REFRESH;
                        end
                    end else if (i_req_valid) begin
                        // Latch the request — common to all three paths
                        req_we_r       <= i_req_we;
                        req_wdata_r    <= i_req_wdata;
                        req_byte_en_r  <= i_req_byte_en;
                        req_bank       <= addr_bank;
                        req_row        <= addr_row;
                        req_col        <= addr_col;
                        o_req_ready    <= 1'b1;

                        if (row_hit) begin
                            // Open-row hit: skip ACTIVATE.  S_ACT will
                            // see wait_cnt=0 next cycle and immediately
                            // issue the RD/WR command.
                            wait_cnt <= '0;
                            state    <= S_ACT;
                        end else if (open_valid) begin
                            // Row conflict: close the open row, then
                            // ACTIVATE the new one.
                            o_phy_cmd   <= SDRAM_CMD_PRECHARGE;
                            o_phy_a     <= '0;
                            o_phy_a[10] <= 1'b1;     // PRECHARGE ALL (only one row open)
                            wait_cnt    <= CNT_BITS'(T_RP - 1);
                            open_valid  <= 1'b0;
                            state       <= S_PRECHARGE_TO_ACT;
                        end else begin
                            // Cold: no row open, ACTIVATE this cycle.
                            o_phy_cmd <= SDRAM_CMD_ACTIVATE;
                            o_phy_a   <= addr_row;
                            o_phy_ba  <= addr_bank;
                            wait_cnt  <= CNT_BITS'(T_RCD - 1);
                            state     <= S_ACT;
                        end
                    end
                end

                S_REFRESH: begin
                    if (wait_cnt == 0) state    <= S_IDLE;
                    else                wait_cnt <= wait_cnt - 1'b1;
                end

                // ── ACTIVATE wait ───────────────────────────
                S_ACT: begin
                    if (wait_cnt == 0) begin
                        // Issue READ or WRITE.  Auto-precharge is OFF
                        // (A10=0) — open-row tracking keeps the row open
                        // for the next request to potentially hit.
                        // The BL=2 burst MUST start on an even col so beat 0
                        // is the low half-word of the CPU 32-bit word.  Force
                        // col[0]=0 even for sub-word accesses (e.g. STB at
                        // byte offset 2 has addr[1]=1, which would otherwise
                        // start the burst on the upper-half col and roll
                        // beat 1 into the *next* word.)
                        o_phy_a                <= '0;
                        o_phy_a[COL_BITS-1:0]  <= {req_col[COL_BITS-1:1], 1'b0};
                        o_phy_a[10]            <= 1'b0;     // no auto-precharge
                        o_phy_ba               <= req_bank;
                        beat                   <= 1'b0;

                        // Mark this bank+row as the open row.  Same
                        // assignment whether we got here via cold ACT or
                        // open-row hit: in both cases the row is now active.
                        open_valid             <= 1'b1;
                        open_bank              <= req_bank;
                        open_row               <= req_row;

                        if (req_we_r) begin
                            o_phy_cmd    <= SDRAM_CMD_WRITE;
                            // Beat 0 of write goes out on DQ this cycle
                            o_phy_dq_oe  <= 1'b1;
                            o_phy_dq_out <= req_wdata_r[15:0];
                            o_phy_dqm    <= ~req_byte_en_r[1:0];
                            beat         <= 1'b1;     // S_RW will issue beat 1
                            state        <= S_RW;
                        end else begin
                            o_phy_cmd    <= SDRAM_CMD_READ;
                            o_phy_dqm    <= 2'b00;    // unmask reads
                            cl_cnt       <= 4'(PHY_OUT_LATENCY+CAS_LATENCY+PHY_IN_LATENCY);
                            state        <= S_RW;
                        end
                    end else begin
                        wait_cnt <= wait_cnt - 1'b1;
                    end
                end

                // ── RW: drive write beat 1 / capture read beats ──
                S_RW: begin
                    o_phy_dqm <= 2'b00;
                    if (req_we_r) begin
                        // Beat 1 of write — data committed this cycle.
                        // From the cache's POV the write is "done"; the
                        // controller still needs T_WR before another
                        // request can issue (a follow-up PRECHARGE on
                        // a row conflict has to land >= T_WR after the
                        // last write data).  Row stays open.
                        o_phy_dq_oe  <= 1'b1;
                        o_phy_dq_out <= req_wdata_r[31:16];
                        o_phy_dqm    <= ~req_byte_en_r[3:2];
                        wait_cnt     <= CNT_BITS'(T_WR);
                        done_r       <= 1'b1;
                        state        <= S_RECOVER;
                    end else begin
                        if (cl_cnt != 0) begin
                            cl_cnt <= cl_cnt - 1'b1;
                        end else begin
                            beat <= ~beat;
                            if (!beat) begin
                                rd_low <= i_phy_dq_in;
                            end else begin
                                // Last beat of the read burst captured.
                                // Row stays open — no T_RP, return to
                                // IDLE immediately so a same-row hit
                                // can issue the next RD on the next cycle.
                                rdata_reg   <= {i_phy_dq_in, rd_low};
                                rsp_valid_r <= 1'b1;
                                done_r      <= 1'b1;
                                wait_cnt    <= '0;
                                state       <= S_RECOVER;
                            end
                        end
                    end
                end

                // ── BURST: reserved for BL>2 (step 6) ───────
                S_BURST: begin
                    state <= S_RECOVER;
                end

                S_RECOVER: begin
                    if (wait_cnt == 0) state    <= S_IDLE;
                    else                wait_cnt <= wait_cnt - 1'b1;
                end

                // ── PRECHARGE then ACT (row-conflict path) ──
                S_PRECHARGE_TO_ACT: begin
                    if (wait_cnt == 0) begin
                        o_phy_cmd <= SDRAM_CMD_ACTIVATE;
                        o_phy_a   <= req_row;
                        o_phy_ba  <= req_bank;
                        wait_cnt  <= CNT_BITS'(T_RCD - 1);
                        state     <= S_ACT;
                    end else begin
                        wait_cnt <= wait_cnt - 1'b1;
                    end
                end

                // ── PRECHARGE then REFRESH (refresh-with-open-row) ──
                S_PRECHARGE_TO_REFRESH: begin
                    if (wait_cnt == 0) begin
                        o_phy_cmd <= SDRAM_CMD_REFRESH;
                        wait_cnt  <= CNT_BITS'(T_RFC);
                        state     <= S_REFRESH;
                    end else begin
                        wait_cnt <= wait_cnt - 1'b1;
                    end
                end

                default: state <= S_INIT_WAIT;
            endcase
        end
    end

endmodule
