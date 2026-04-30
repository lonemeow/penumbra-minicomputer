// SDR SDRAM controller core — generic, single-domain, single-word interface.
//
// Drives a per-FPGA-family PHY (sdram_phy_ecp5 in hardware,
// sdram_phy_sim in Verilator).  Parameterized over chip geometry and
// timing; presets in sdram_pkg.sv.
//
// First-iteration policy: close-after-access (auto-precharge via A10=1
// on every RD/WR).  No open-row tracking, no per-access pipelining.
// FSM states are separated so step 6 of the rollout (open-row + AP
// off) is a localized change, not a rewrite.
//
// See doc/internals/sdram-controller.md for the design plan.

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
    /* verilator lint_off UNUSEDSIGNAL */
    wire _unused = &{1'b0, i_rsp_ready,
                     i_req_addr[31:COL_BITS+ROW_BITS+BA_BITS+1],
                     i_req_addr[0]};
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
        S_INIT_WAIT,    // T_POWERUP power-up delay
        S_INIT_PRE,     // PRECHARGE ALL
        S_INIT_REF1,    // first AUTO_REFRESH
        S_INIT_REF2,    // second AUTO_REFRESH
        S_INIT_MRS,     // MODE REGISTER SET
        S_IDLE,
        S_REFRESH,
        S_ACT,          // wait T_RCD after ACTIVATE
        S_RW,           // RD/WR command issued; first beat
        S_BURST,        // (reserved for BL>2 in step 6)
        S_RECOVER       // wait T_RP / T_WR before next command
    } state_t;

    state_t state;

    // ── Counters ────────────────────────────────────────────
    localparam int CNT_BITS  = $clog2(T_POWERUP + 1) + 1;
    localparam int REFI_BITS = $clog2(T_REFI + 1) + 1;

    logic [CNT_BITS-1:0]   wait_cnt;
    logic [REFI_BITS-1:0]  refresh_cnt;
    logic                  refresh_pending;

    // ── Latched request ─────────────────────────────────────
    // req_row is unused today (the row goes directly into o_phy_a on
    // ACTIVATE issue).  It's kept for step 6, where same-bank+row hit
    // detection needs to compare an incoming request's row against the
    // most recently activated row.
    /* verilator lint_off UNUSEDSIGNAL */
    logic                  req_we_r;
    logic [31:0]           req_wdata_r;
    logic [3:0]            req_byte_en_r;
    logic [BA_BITS-1:0]    req_bank;
    logic [ROW_BITS-1:0]   req_row;
    logic [COL_BITS-1:0]   req_col;
    /* verilator lint_on UNUSEDSIGNAL */

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
                        o_phy_cmd <= SDRAM_CMD_REFRESH;
                        wait_cnt  <= CNT_BITS'(T_RFC);
                        state     <= S_REFRESH;
                    end else if (i_req_valid) begin
                        // Latch request and issue ACTIVATE in this cycle
                        req_we_r       <= i_req_we;
                        req_wdata_r    <= i_req_wdata;
                        req_byte_en_r  <= i_req_byte_en;
                        req_bank       <= addr_bank;
                        req_row        <= addr_row;
                        req_col        <= addr_col;

                        o_phy_cmd      <= SDRAM_CMD_ACTIVATE;
                        o_phy_a        <= addr_row;
                        o_phy_ba       <= addr_bank;
                        wait_cnt       <= CNT_BITS'(T_RCD - 1);
                        o_req_ready    <= 1'b1;
                        state          <= S_ACT;
                    end
                end

                S_REFRESH: begin
                    if (wait_cnt == 0) state    <= S_IDLE;
                    else                wait_cnt <= wait_cnt - 1'b1;
                end

                // ── ACTIVATE wait ───────────────────────────
                S_ACT: begin
                    if (wait_cnt == 0) begin
                        // Issue READ or WRITE with auto-precharge.
                        // The BL=2 burst MUST start on an even col so beat 0
                        // is the low half-word of the CPU 32-bit word.  Force
                        // col[0]=0 even for sub-word accesses (e.g. STB at
                        // byte offset 2 has addr[1]=1, which would otherwise
                        // start the burst on the upper-half col and roll
                        // beat 1 into the *next* word.)
                        o_phy_a                <= '0;
                        o_phy_a[COL_BITS-1:0]  <= {req_col[COL_BITS-1:1], 1'b0};
                        o_phy_a[10]            <= 1'b1;     // auto-precharge
                        o_phy_ba               <= req_bank;
                        beat                   <= 1'b0;

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
                        // controller still needs T_WR + T_RP recovery
                        // before accepting another request.
                        o_phy_dq_oe  <= 1'b1;
                        o_phy_dq_out <= req_wdata_r[31:16];
                        o_phy_dqm    <= ~req_byte_en_r[3:2];
                        wait_cnt     <= CNT_BITS'(T_WR + T_RP);
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
                                rdata_reg   <= {i_phy_dq_in, rd_low};
                                rsp_valid_r <= 1'b1;
                                done_r      <= 1'b1;
                                wait_cnt    <= CNT_BITS'(T_RP);
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

                default: state <= S_INIT_WAIT;
            endcase
        end
    end

endmodule
