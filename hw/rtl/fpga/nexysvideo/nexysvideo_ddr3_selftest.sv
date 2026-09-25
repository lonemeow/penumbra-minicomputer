// Nexys Video DDR3 self-test sequencer: repeated calibration runs, logged
//
// Drives nexysvideo_ddr3 through one self-test after another and prints
// a line per run on a serial line (8N1). A run holds the controller in
// reset, releases it, and times it until calibration completes; the
// controller's self-test is the last step of calibration, so a completed
// run has also written and read back the whole chip. The self-test's
// counters, read over the controller's debug port, give how many reads
// it checked and how many came back wrong. Runs repeat until reset, so
// a board left running logs the chip from cold to its working
// temperature.
//
// The counters count from configuration and survive every reset, the
// controller's and this sequencer's, so a run reads them as it starts
// (just after release, long before the self-test begins) and again as it
// ends, and reports the difference: reads of a run cut short by a reset
// belong to no run. A mismatch before calibration completes resets the
// controller into another attempt: a run can complete after mismatches,
// and one that never completes ends at TIMEOUT_MS instead.
//
// The debug port registers (UberDDR3, ddr3_controller.v): 15 holds the
// reads that matched, 16 the reads that did not.

module nexysvideo_ddr3_selftest #(
    parameter longint CLK_HZ     = 83_333_333,
    parameter longint BAUD       = 115_200,
    parameter int     BIST_MODE  = 2,        // what the controller runs, for the banner
    parameter int     HOLD_MS    = 1,        // controller reset between runs
    parameter int     TIMEOUT_MS = 300_000,  // a run that has not calibrated by then fails
    parameter int     PAUSE_MS   = 1_000     // between one run's line and the next run
) (
    input  logic        i_clk,
    input  logic        i_rst,

    output logic        o_ctrl_rst_n,       // the controller's reset
    input  logic        i_calib_complete,

    // The controller's debug port, reads only.
    output logic        o_dbg_stb,
    output logic [6:0]  o_dbg_addr,
    input  logic        i_dbg_stall,
    input  logic        i_dbg_ack,
    input  logic [31:0] i_dbg_data,

    output logic        o_tx,               // serial line, idle high

    output logic        o_testing,          // a run in progress
    output logic        o_last_pass,        // the last finished run passed
    output logic        o_mismatch_seen     // a run has seen a mismatch since reset
);
    localparam logic [6:0] DBG_READS_MATCHED    = 7'd15;
    localparam logic [6:0] DBG_READS_MISMATCHED = 7'd16;

    // ── Run verdict ─────────────────────────────────────────────
    typedef enum logic [1:0] {
        V_PASS,     // calibrated, the self-test clean
        V_RETRIED,  // calibrated, after attempts the self-test failed
        V_FAIL      // never calibrated within TIMEOUT_MS, or calibrated
                    // without the self-test checking a single read
    } verdict_t;

    // What a finished run showed. calibrated: calibration completed
    // before the timeout. matched, mismatched: the self-test's reads that
    // came back right and wrong during this run.
    function automatic verdict_t run_verdict(input logic        calibrated,
                                             input logic [31:0] matched,
                                             input logic [31:0] mismatched);
        if (!calibrated) return V_FAIL;
        if (matched == 0) return V_FAIL;
        if (mismatched != 0) return V_RETRIED;
        return V_PASS;
    endfunction

    // ── Timebase ────────────────────────────────────────────────
    localparam int MS_CYCLES = int'(CLK_HZ / 1000);
    localparam int MS_W      = $clog2(MS_CYCLES);

    logic [MS_W-1:0] ms_div_q;
    logic            ms_tick;

    assign ms_tick = (ms_div_q == MS_W'(MS_CYCLES - 1));

    always_ff @(posedge i_clk) begin
        if (i_rst || ms_tick)
            ms_div_q <= '0;
        else
            ms_div_q <= ms_div_q + MS_W'(1);
    end

    // ── Serial transmitter ──────────────────────────────────────
    // One frame at a time: start bit, eight data bits LSB first, stop.
    localparam int                BIT_CYCLES = int'((CLK_HZ + BAUD / 2) / BAUD);
    localparam int                TICK_W     = $clog2(BIT_CYCLES);
    localparam logic [TICK_W-1:0] TICK_LAST  = TICK_W'(BIT_CYCLES - 1);

    logic              tx_load;
    logic [7:0]        tx_byte;
    logic              tx_idle;
    logic [9:0]        tx_frame_q;
    logic [3:0]        tx_bits_q;
    logic [TICK_W-1:0] tx_tick_q;

    assign tx_idle = (tx_bits_q == 4'd0);

    // After reset the line stays idle for two frames before the first
    // start bit. A reset in the middle of a frame cuts it short, and the
    // receiver at the other end finds frame boundaries again only in an
    // idle gap; without one, the banner's back-to-back characters would
    // arrive misframed.
    localparam int GUARD_CYCLES = 20 * BIT_CYCLES;
    localparam int GUARD_W      = $clog2(GUARD_CYCLES + 1);

    logic [GUARD_W-1:0] guard_q;
    logic               tx_ready;

    assign tx_ready = (guard_q == '0);

    always_ff @(posedge i_clk) begin
        if (i_rst)
            guard_q <= GUARD_W'(GUARD_CYCLES);
        else if (!tx_ready)
            guard_q <= guard_q - GUARD_W'(1);
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_frame_q <= '1;
            tx_bits_q  <= 4'd0;
            tx_tick_q  <= '0;
        end else if (tx_load) begin
            tx_frame_q <= {1'b1, tx_byte, 1'b0};
            tx_bits_q  <= 4'd10;
            tx_tick_q  <= '0;
        end else if (!tx_idle) begin
            if (tx_tick_q == TICK_LAST) begin
                tx_frame_q <= {1'b1, tx_frame_q[9:1]};
                tx_bits_q  <= tx_bits_q - 4'd1;
                tx_tick_q  <= '0;
            end else begin
                tx_tick_q <= tx_tick_q + TICK_W'(1);
            end
        end
    end

    assign o_tx = tx_frame_q[0];

    // ── Lines ───────────────────────────────────────────────────
    // A line is built whole, then sent from its first character.
    function automatic logic [7:0] hex_char(input logic [3:0] n);
        return (n < 4'd10) ? 8'("0") + 8'(n) : 8'("A") + 8'(n) - 8'd10;
    endfunction

    function automatic logic [63:0] hex32(input logic [31:0] v);
        return {hex_char(v[31:28]), hex_char(v[27:24]), hex_char(v[23:20]),
                hex_char(v[19:16]), hex_char(v[15:12]), hex_char(v[11:8]),
                hex_char(v[7:4]),   hex_char(v[3:0])};
    endfunction

    function automatic logic [31:0] verdict_word(input verdict_t v);
        case (v)
            V_PASS:    return "pass";
            V_RETRIED: return "RTRY";
            default:   return "FAIL";
        endcase
    endfunction

    localparam BANNER = {"\015\nNexys Video DDR3 self-test: MT41K256M16 (x16), BIST mode ",
                         8'("0") + 8'(BIST_MODE), "\015\n"};
    localparam int BANNER_BYTES = $bits(BANNER) / 8;

    // A run's line, as S_BUILD assembles it: the run number and three
    // counts in hex, the verdict as a four-letter word.
    localparam int REPORT_BYTES = $bits({"run ", 16'h0, ": ", 32'h0,
                                         " reads ", 64'h0,
                                         " mismatches ", 64'h0,
                                         " calibrated ", 64'h0, " ms\015\n"}) / 8;
    localparam int LINE_BYTES   = (BANNER_BYTES > REPORT_BYTES) ? BANNER_BYTES : REPORT_BYTES;
    localparam int LINE_W       = $clog2(LINE_BYTES + 1);

    logic [8*LINE_BYTES-1:0] line_q;      // next character at the top
    logic [LINE_W-1:0]       line_left_q; // characters still to send

    // ── Sequencer ───────────────────────────────────────────────
    typedef enum logic [3:0] {
        S_BANNER,           // the banner going out, once after reset
        S_HOLD,             // controller held in reset between runs
        S_START_MATCHED,    // the counters as the run starts
        S_START_MISMATCHED,
        S_CALIB,            // controller running, waiting for calibration
        S_END_MATCHED,      // the counters as the run ends
        S_END_MISMATCHED,
        S_BUILD,            // the run's line built, one cycle
        S_SEND,             // the run's line going out
        S_PAUSE
    } state_t;

    state_t      state_q;
    logic [31:0] ms_q;            // milliseconds into the current state
    logic [7:0]  run_q;
    logic        calibrated_q;
    logic [31:0] calib_ms_q;      // from release to calibration complete
    logic [31:0] matched_start_q, mismatched_start_q;
    logic [31:0] matched_end_q,   mismatched_end_q;
    logic        dbg_issued_q;
    logic [9:0]  dbg_wait_q;      // cycles since the read was issued
    logic        dbg_done;

    verdict_t    verdict;
    logic [31:0] matched_run, mismatched_run;
    logic [15:0] run_hex;

    assign run_hex = {hex_char(run_q[7:4]), hex_char(run_q[3:0])};

    assign matched_run    = matched_end_q - matched_start_q;
    assign mismatched_run = mismatched_end_q - mismatched_start_q;
    assign verdict        = run_verdict(calibrated_q, matched_run, mismatched_run);

    // A debug read the controller lost (its debug port restarts with
    // every reset it takes) is issued again after a while.
    logic dbg_reading, dbg_reading_matched;

    assign dbg_reading         = state_q == S_START_MATCHED || state_q == S_START_MISMATCHED
                                 || state_q == S_END_MATCHED || state_q == S_END_MISMATCHED;
    assign dbg_reading_matched = state_q == S_START_MATCHED || state_q == S_END_MATCHED;
    assign dbg_done            = dbg_issued_q && i_dbg_ack;
    assign o_dbg_stb           = dbg_reading && !dbg_issued_q;
    assign o_dbg_addr          = dbg_reading_matched ? DBG_READS_MATCHED : DBG_READS_MISMATCHED;

    assign tx_load = tx_ready && tx_idle && (line_left_q != '0);
    assign tx_byte = line_q[8*LINE_BYTES-1 -: 8];

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q           <= S_BANNER;
            ms_q              <= '0;
            run_q             <= 8'd1;
            calibrated_q      <= 1'b0;
            calib_ms_q         <= '0;
            matched_start_q    <= '0;
            mismatched_start_q <= '0;
            matched_end_q      <= '0;
            mismatched_end_q   <= '0;
            dbg_issued_q       <= 1'b0;
            dbg_wait_q        <= '0;
            line_q            <= {BANNER, {(8*(LINE_BYTES - BANNER_BYTES)){1'b0}}};
            line_left_q       <= LINE_W'(BANNER_BYTES);
            o_last_pass       <= 1'b0;
            o_mismatch_seen   <= 1'b0;
        end else begin
            if (ms_tick)
                ms_q <= ms_q + 32'd1;
            if (tx_load) begin
                line_q      <= {line_q[8*LINE_BYTES-9:0], 8'h00};
                line_left_q <= line_left_q - LINE_W'(1);
            end

            if (o_dbg_stb && !i_dbg_stall) begin
                dbg_issued_q <= 1'b1;
                dbg_wait_q   <= '0;
            end else if (dbg_issued_q && !i_dbg_ack) begin
                dbg_wait_q <= dbg_wait_q + 10'd1;
                if (dbg_wait_q == '1)
                    dbg_issued_q <= 1'b0;
            end

            case (state_q)
                S_BANNER: if (line_left_q == '0 && tx_idle) begin
                    state_q <= S_HOLD;
                    ms_q    <= '0;
                end
                // Release; the run's time counts from here.
                S_HOLD: if (ms_q == 32'(HOLD_MS)) begin
                    state_q <= S_START_MATCHED;
                    ms_q    <= '0;
                end
                S_START_MATCHED: if (dbg_done) begin
                    state_q         <= S_START_MISMATCHED;
                    matched_start_q <= i_dbg_data;
                    dbg_issued_q    <= 1'b0;
                end
                S_START_MISMATCHED: if (dbg_done) begin
                    state_q            <= S_CALIB;
                    mismatched_start_q <= i_dbg_data;
                    dbg_issued_q       <= 1'b0;
                end
                S_CALIB: if (i_calib_complete || ms_q == 32'(TIMEOUT_MS)) begin
                    state_q      <= S_END_MATCHED;
                    calibrated_q <= i_calib_complete;
                    calib_ms_q   <= ms_q;
                end
                S_END_MATCHED: if (dbg_done) begin
                    state_q       <= S_END_MISMATCHED;
                    matched_end_q <= i_dbg_data;
                    dbg_issued_q  <= 1'b0;
                end
                S_END_MISMATCHED: if (dbg_done) begin
                    state_q          <= S_BUILD;
                    mismatched_end_q <= i_dbg_data;
                    dbg_issued_q     <= 1'b0;
                end
                S_BUILD: begin
                    state_q           <= S_SEND;
                    line_q            <= {"run ", run_hex, ": ", verdict_word(verdict),
                                          " reads ", hex32(matched_run),
                                          " mismatches ", hex32(mismatched_run),
                                          " calibrated ", hex32(calib_ms_q), " ms\015\n",
                                          {(8*(LINE_BYTES - REPORT_BYTES)){1'b0}}};
                    line_left_q       <= LINE_W'(REPORT_BYTES);
                    o_last_pass       <= (verdict == V_PASS);
                    o_mismatch_seen   <= o_mismatch_seen || (mismatched_run != '0);
                end
                S_SEND: if (line_left_q == '0 && tx_idle) begin
                    state_q <= S_PAUSE;
                    ms_q    <= '0;
                    run_q   <= run_q + 8'd1;
                end
                S_PAUSE: if (ms_q == 32'(PAUSE_MS)) begin
                    state_q <= S_HOLD;
                    ms_q    <= '0;
                end
                default: state_q <= S_BANNER;
            endcase
        end
    end

    assign o_ctrl_rst_n = !(i_rst || state_q == S_BANNER || state_q == S_HOLD);
    assign o_testing    = (state_q == S_CALIB);

endmodule
