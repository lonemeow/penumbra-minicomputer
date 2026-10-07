// I2C bus master, one byte per command.
//
// A command writes or reads one byte, with a START (or a repeated START)
// before it and a STOP after it where asked, and completes with one
// response. The consumer strings commands into transactions: address,
// data bytes, a repeated START ahead of a read, a NACK on the last byte
// read. A write the slave does not acknowledge ends the transaction
// with a STOP of the master's own, since nothing useful can follow it.
//
// One state machine draws the bus, and its states are the steps of the
// bus's timing diagram: each state lasts one step, a quarter of the SCL
// period. At 100 kHz a step is 2.5 us, and two of them meet every
// standard-mode minimum (4.7 us low and START setup, 4.0 us high and
// START hold). The table below gives SCL and SDA in every state, 1 for
// a line let go and 0 for one pulled low; only the pins turn that into
// open-drain drive.
//
// Every symbol ends by pulling SCL low itself, so the next one begins
// with SCL low and SDA free to move, and between two bytes the bus
// rests with SCL held low (S_HOLD). Only a STOP ends with SCL high,
// since it hands the bus back. A START on a free bus and a repeated
// START inside a transaction are separate sequences: the repeated one
// first brings both lines back up, then joins the other.
//
// It is the only master, and it does not wait for a slave that
// stretches the clock. SCL read low while released, or SDA read low
// where a START needs it high, ends the command with a bus error, so a
// stuck bus fails a command rather than hanging it.
//
// So a response ends its transaction in three cases:
//
//   * the command it answers carried a STOP;
//   * that command was a write the slave did not acknowledge
//     (o_rsp_nack), and the master has put its own STOP behind it;
//   * it reports a bus error (o_rsp_error).
//
// Behind any of them the master holds no transaction and has let both
// lines go, and the next command needs a START. An i2c_shared_master
// gives several consumers a command port each on that rule.
//
// After reset the master frees the bus before it takes a command. A
// reset can land in the middle of a transfer and leave a slave part way
// through a byte: one that was sending still holds SDA and is clocked
// until it lets go, nine pulses at most, then given a STOP; with SDA
// free, a START and a STOP reset any slave that was receiving without
// clocking a bit into it.

module i2c_master #(
    parameter int CLK_HZ = 60_000_000,
    parameter int SCL_HZ = 100_000        // a ceiling: SCL never runs above it
)(
    input  logic       i_clk,
    input  logic       i_rst,

    // ── Command: one byte ────────────────────────────────────
    input  logic       i_cmd_valid,
    output logic       o_cmd_ready,
    input  logic       i_cmd_start,   // START, or a repeated START, first
    input  logic       i_cmd_stop,    // STOP after the byte
    input  logic       i_cmd_read,    // read the byte rather than write it
    input  logic [7:0] i_cmd_wdata,
    input  logic       i_cmd_nack,    // a read: answer the byte with NACK

    // ── Response: one pulse per command ──────────────────────
    output logic       o_rsp_valid,
    output logic [7:0] o_rsp_rdata,
    output logic       o_rsp_nack,    // a write: the slave did not acknowledge
    output logic       o_rsp_error,   // the bus would not move; nothing else holds

    // ── Pins ─────────────────────────────────────────────────
    // o_scl and o_sda are the levels the master asks for: 1 lets the
    // line go high, 0 pulls it low. i_scl and i_sda are what the bus
    // carries.
    output logic       o_scl,
    input  logic       i_scl,
    output logic       o_sda,
    input  logic       i_sda
);

    // Clocks in a step, a quarter of the SCL period, rounded up.
    localparam int STEP_CYCLES = (CLK_HZ + 4 * SCL_HZ - 1) / (4 * SCL_HZ);
    localparam int DIV_W       = $clog2(STEP_CYCLES);

    // A released line is read back at the end of the step it is released
    // in, through the line register and two synchroniser stages.
    build_check #(
        .COND (STEP_CYCLES >= 8),
        .MSG  ("i2c_master: CLK_HZ leaves a step of SCL_HZ too short to read the lines back")
    ) u_check_step ();

    // Equal steps give a low time of half the period, which fast mode's
    // 1.3 us minimum allows up to here.
    build_check #(
        .COND (SCL_HZ <= 384_000),
        .MSG  ("i2c_master: SCL_HZ too high for fast mode's low time with equal steps")
    ) u_check_low_time ();

    // ── Pins in ──────────────────────────────────────────────
    // ASYNC_REG on every synchroniser flop: Vivado places each pair
    // together and its clock-crossing check recognises it.
    (* ASYNC_REG = "TRUE" *) logic scl_s1_q, scl_s2_q;
    (* ASYNC_REG = "TRUE" *) logic sda_s1_q, sda_s2_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            {scl_s2_q, scl_s1_q} <= 2'b11;
            {sda_s2_q, sda_s1_q} <= 2'b11;
        end else begin
            {scl_s2_q, scl_s1_q} <= {scl_s1_q, i_scl};
            {sda_s2_q, sda_s1_q} <= {sda_s1_q, i_sda};
        end
    end

    // ── States ───────────────────────────────────────────────
    typedef enum logic [4:0] {
        S_CLEAR_1,              // after reset: let the lines and
        S_CLEAR_2,              //   synchronisers settle, then look at SDA
        S_CLEAR_3,              // SDA held: SCL down ahead of the first pulse
        S_IDLE,                 // bus free
        S_HOLD,                 // between two bytes, SCL held low
        S_START_1,
        S_START_2,
        S_START_3,
        S_REPEATED_START_1,
        S_REPEATED_START_2,
        S_REPEATED_START_3,
        S_BIT_1,
        S_BIT_2,
        S_BIT_3,
        S_BIT_4,
        S_STOP_1,
        S_STOP_2,
        S_STOP_3,
        S_STOP_4,
        S_STOP_5
    } state_e;

    state_e           state_q;
    logic [DIV_W-1:0] div_q;
    logic             clearing_q;       // the bus clear after reset is running
    logic [3:0]       clear_pulses_q;
    logic             clear_released_q; // the clear's last pulse saw SDA high
    logic [3:0]       bit_idx_q;        // 0-7 the byte, 8 its acknowledge

    logic             cmd_stop_q;
    logic             cmd_read_q;
    logic             cmd_nack_q;
    logic [7:0]       wdata_q;          // the byte going out, next bit at the top
    logic [7:0]       rdata_q;
    logic             nack_q;
    logic             rsp_valid_q;
    logic             rsp_error_q;

    logic             scl_q;
    logic             sda_q;

    // IDLE and HOLD wait on a command; every other state lasts one step.
    logic waiting;
    logic step_end;
    assign waiting  = (state_q == S_IDLE) || (state_q == S_HOLD);
    assign step_end = !waiting && (div_q == DIV_W'(STEP_CYCLES - 1));

    // SDA for the bit on the bus. The clear only clocks, a byte read is
    // the slave's to drive, and the ninth bit is the acknowledge: the
    // slave's on a write, the master's on a read (0 for ACK).
    logic bit_sda;
    always_comb begin
        if (clearing_q)
            bit_sda = 1'b1;
        else if (bit_idx_q == 4'd8)
            bit_sda = !cmd_read_q || cmd_nack_q;
        else if (cmd_read_q)
            bit_sda = 1'b1;
        else
            bit_sda = wdata_q[7];
    end

    // ── The lines in each state ──────────────────────────────
    // START, on a free bus:
    //              1   2   3
    //   SCL    ‾‾‾‾‾‾‾‾‾‾‾‾\___
    //   SDA    ‾‾‾‾\___________
    //
    // Repeated START, from a held bus, then on into S_START_1:
    //              1   2   3
    //   SCL    ________/‾‾‾‾‾‾‾
    //   SDA    ====X‾‾‾‾‾‾‾‾‾‾‾
    //
    // A bit, SDA from bit_sda:
    //              1   2   3   4
    //   SCL    ________/‾‾‾‾‾‾‾\___
    //   SDA    ====X===============
    //
    // STOP, from a held bus:
    //              1   2   3   4   5
    //   SCL    ________/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
    //   SDA    ====X___________/‾‾‾‾‾‾‾
    logic scl_d;
    logic sda_d;

    always_comb begin
        case (state_q)
            S_HOLD:             {scl_d, sda_d} = {1'b0, bit_sda};
            S_CLEAR_3:          {scl_d, sda_d} = 2'b01;   // a bit enters with SCL low

            S_START_1,
            S_START_2:          {scl_d, sda_d} = 2'b10;   // the START, and its hold
            S_START_3:          {scl_d, sda_d} = 2'b00;

            S_REPEATED_START_1: {scl_d, sda_d} = 2'b01;   // SDA up under a low SCL
            S_REPEATED_START_2,
            S_REPEATED_START_3: {scl_d, sda_d} = 2'b11;   // the START's setup

            S_BIT_1,
            S_BIT_4:            {scl_d, sda_d} = {1'b0, bit_sda};
            S_BIT_2,
            S_BIT_3:            {scl_d, sda_d} = {1'b1, bit_sda};

            S_STOP_1:           {scl_d, sda_d} = 2'b00;
            S_STOP_2,
            S_STOP_3:           {scl_d, sda_d} = 2'b10;   // the STOP's setup

            default:            {scl_d, sda_d} = 2'b11;   // free bus: clear, idle,
                                                          // the STOP and after it
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            scl_q <= 1'b1;
            sda_q <= 1'b1;
        end else begin
            scl_q <= scl_d;
            sda_q <= sda_d;
        end
    end

    assign o_scl = scl_q;
    assign o_sda = sda_q;

    // ── Reading the bus back ─────────────────────────────────
    // A line is read at the end of the first step it should be high in.
    // SCL low there is a slave stretching the clock or a stuck bus; SDA
    // low before a START has it, or a slave still holding it.
    logic scl_released;
    logic scl_stuck;
    logic sda_stuck;

    assign scl_released = (state_q == S_REPEATED_START_2) ||
                          (state_q == S_BIT_2) ||
                          (state_q == S_STOP_2);
    assign scl_stuck = step_end && scl_released && !scl_s2_q;
    assign sda_stuck = step_end && (state_q == S_REPEATED_START_3) && !sda_s2_q;

    // ── Sequencing ───────────────────────────────────────────
    logic cmd_take;
    assign cmd_take = i_cmd_valid && waiting && !clearing_q;

    always_ff @(posedge i_clk) begin
        rsp_valid_q <= 1'b0;

        if (i_rst) begin
            state_q          <= S_CLEAR_1;
            div_q            <= '0;
            clearing_q       <= 1'b1;
            clear_pulses_q   <= 4'd0;
            clear_released_q <= 1'b0;
            bit_idx_q        <= 4'd0;
            cmd_stop_q       <= 1'b0;
            cmd_read_q       <= 1'b0;
            cmd_nack_q       <= 1'b0;
            wdata_q          <= 8'd0;
            rdata_q          <= 8'd0;
            nack_q           <= 1'b0;
            rsp_error_q      <= 1'b0;
        end else begin
            div_q <= (waiting || step_end) ? '0 : div_q + 1'b1;

            if (cmd_take) begin
                cmd_stop_q <= i_cmd_stop;
                cmd_read_q <= i_cmd_read;
                cmd_nack_q <= i_cmd_nack;
                wdata_q    <= i_cmd_wdata;
                bit_idx_q  <= 4'd0;
                nack_q     <= 1'b0;
                if (state_q == S_HOLD) begin
                    state_q <= i_cmd_start ? S_REPEATED_START_1 : S_BIT_1;
                end else if (!scl_s2_q || !sda_s2_q) begin
                    // A START on a bus that is not free has nothing to
                    // pull down.
                    rsp_valid_q <= 1'b1;
                    rsp_error_q <= 1'b1;
                end else begin
                    state_q <= S_START_1;
                end
            end else if (scl_stuck || sda_stuck) begin
                state_q <= S_IDLE;
                if (clearing_q) begin
                    clearing_q <= 1'b0;
                end else begin
                    rsp_valid_q <= 1'b1;
                    rsp_error_q <= 1'b1;
                end
            end else if (step_end) begin
                // SDA is read a step into SCL's high time.
                if (state_q == S_BIT_2) begin
                    if (clearing_q)
                        clear_released_q <= sda_s2_q;
                    else if (bit_idx_q != 4'd8)
                        rdata_q <= {rdata_q[6:0], sda_s2_q};
                    else if (!cmd_read_q)
                        nack_q <= sda_s2_q;
                end

                case (state_q)
                    S_CLEAR_1:          state_q <= S_CLEAR_2;
                    S_CLEAR_2:          state_q <= sda_s2_q ? S_START_1 : S_CLEAR_3;
                    S_CLEAR_3:          state_q <= S_BIT_1;

                    S_START_1:          state_q <= S_START_2;
                    S_START_2:          state_q <= S_START_3;
                    S_START_3:          state_q <= clearing_q ? S_STOP_1 : S_BIT_1;

                    S_REPEATED_START_1: state_q <= S_REPEATED_START_2;
                    S_REPEATED_START_2: state_q <= S_REPEATED_START_3;
                    S_REPEATED_START_3: state_q <= S_START_1;

                    S_BIT_1:            state_q <= S_BIT_2;
                    S_BIT_2:            state_q <= S_BIT_3;
                    S_BIT_3:            state_q <= S_BIT_4;
                    S_BIT_4: begin
                        if (clearing_q) begin
                            if (clear_released_q || clear_pulses_q == 4'd8) begin
                                state_q <= S_STOP_1;
                            end else begin
                                clear_pulses_q <= clear_pulses_q + 4'd1;
                                state_q        <= S_BIT_1;
                            end
                        end else if (bit_idx_q != 4'd8) begin
                            bit_idx_q <= bit_idx_q + 4'd1;
                            wdata_q   <= {wdata_q[6:0], 1'b0};
                            state_q   <= S_BIT_1;
                        end else if (cmd_stop_q || (!cmd_read_q && nack_q)) begin
                            state_q <= S_STOP_1;
                        end else begin
                            state_q     <= S_HOLD;
                            rsp_valid_q <= 1'b1;
                            rsp_error_q <= 1'b0;
                        end
                    end

                    S_STOP_1:           state_q <= S_STOP_2;
                    S_STOP_2:           state_q <= S_STOP_3;
                    S_STOP_3:           state_q <= S_STOP_4;
                    S_STOP_4:           state_q <= S_STOP_5;
                    S_STOP_5: begin
                        state_q <= S_IDLE;
                        if (clearing_q) begin
                            clearing_q <= 1'b0;
                        end else begin
                            rsp_valid_q <= 1'b1;
                            rsp_error_q <= 1'b0;
                        end
                    end

                    default: ;
                endcase
            end
        end
    end

    assign o_cmd_ready = waiting && !clearing_q;
    assign o_rsp_valid = rsp_valid_q;
    assign o_rsp_rdata = rdata_q;
    assign o_rsp_nack  = nack_q;
    assign o_rsp_error = rsp_error_q;

    // synthesis translate_off
    // A byte outside a transaction has no slave listening for it.
    always_ff @(posedge i_clk) begin
        if (!i_rst && cmd_take && state_q == S_IDLE)
            assert (i_cmd_start)
                else $error("i2c_master: a byte with no START and no transaction open");
    end
    // synthesis translate_on

endmodule
