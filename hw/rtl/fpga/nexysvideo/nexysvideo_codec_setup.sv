// Brings up the Nexys Video's ADAU1761 codec over I2C, after every reset.
//
// The codec plays the audio device's frames: its PLL makes the 49.152 MHz
// core clock (1024 x 48 kHz) from MCLK_HZ, it drives the serial port's
// bit and frame clocks (BCLK, LRCLK) from that core clock itself, with
// the FPGA following them, the serial input goes straight to the DACs
// with the SigmaDSP off, and the DACs feed the line out at 0 dB and the
// headphones at -10 dB. The steps below are that configuration, one register per step,
// and every register the playback path depends on is written, never
// left to its default: the codec has no reset pin, so it may still hold
// whatever an earlier bitstream gave it, and may still be playing.
//
// A codec still running is muted before its clock stops, so a reset
// does not cut its output mid-sample; one that is not running is
// silent already, and with its core clock off it would accept no
// register but the clock's and the PLL's. Its outputs come up muted,
// and are unmuted only after the precharge the datasheet asks of them
// once they are powered.
//
// The outputs report progress as the audio device's STATUS: busy from
// reset until the last step, then done or faulted. A fault stops the
// sequence where it stands, and its code says what failed and where:
//
//   [7:6] what failed:
//           * 1: the codec did not acknowledge a byte
//           * 2: a bus error, the master reading a line low that it had
//             let go
//           * 3: the PLL did not lock within LOCK_TIMEOUT_MS
//   [5:0] the step it failed in, an index into the table below

module nexysvideo_codec_setup #(
    parameter int CLK_HZ  = 60_000_000,
    parameter int MCLK_HZ = 12_500_000
)(
    input  logic       i_clk,
    input  logic       i_rst,

    // ── To the I2C master ────────────────────────────────────
    output logic       o_cmd_valid,
    input  logic       i_cmd_ready,
    output logic       o_cmd_start,
    output logic       o_cmd_stop,
    output logic       o_cmd_read,
    output logic [7:0] o_cmd_wdata,
    output logic       o_cmd_nack,
    input  logic       i_rsp_valid,
    input  logic [7:0] i_rsp_rdata,
    input  logic       i_rsp_nack,
    input  logic       i_rsp_error,

    // ── Progress, as the audio device's STATUS ───────────────
    output logic       o_busy,
    output logic       o_fault,
    output logic [7:0] o_fault_code
);

    // ── The codec ────────────────────────────────────────────
    // ADR0 and ADR1 are pulled up on the board.
    localparam logic [6:0] CODEC_ADDR = 7'h3B;

    localparam logic [15:0] R0_CLOCK_CONTROL      = 16'h4000;
    localparam logic [15:0] R1_PLL_CONTROL        = 16'h4002;
    localparam logic [15:0] R15_SERIAL_PORT_0     = 16'h4015;
    localparam logic [15:0] R16_SERIAL_PORT_1     = 16'h4016;
    localparam logic [15:0] R17_CONVERTER_0       = 16'h4017;
    localparam logic [15:0] R22_MIXER3_LEFT_0     = 16'h401C;
    localparam logic [15:0] R23_MIXER3_LEFT_1     = 16'h401D;
    localparam logic [15:0] R24_MIXER4_RIGHT_0    = 16'h401E;
    localparam logic [15:0] R25_MIXER4_RIGHT_1    = 16'h401F;
    localparam logic [15:0] R26_MIXER5_LINE_LEFT  = 16'h4020;
    localparam logic [15:0] R27_MIXER6_LINE_RIGHT = 16'h4021;
    localparam logic [15:0] R28_MIXER7_MONO       = 16'h4022;
    localparam logic [15:0] R29_HP_LEFT           = 16'h4023;
    localparam logic [15:0] R30_HP_RIGHT          = 16'h4024;
    localparam logic [15:0] R31_LINE_LEFT         = 16'h4025;
    localparam logic [15:0] R32_LINE_RIGHT        = 16'h4026;
    localparam logic [15:0] R33_MONO              = 16'h4027;
    localparam logic [15:0] R34_POP_CLICK         = 16'h4028;
    localparam logic [15:0] R35_PLAYBACK_POWER    = 16'h4029;
    localparam logic [15:0] R36_DAC_CONTROL_0     = 16'h402A;
    localparam logic [15:0] R37_DAC_VOLUME_LEFT   = 16'h402B;
    localparam logic [15:0] R38_DAC_VOLUME_RIGHT  = 16'h402C;
    localparam logic [15:0] R67_DEJITTER          = 16'h4036;
    localparam logic [15:0] R58_SERIAL_IN_ROUTE   = 16'h40F2;
    localparam logic [15:0] R60_SERIAL_PINS       = 16'h40F4;
    localparam logic [15:0] R61_DSP_ENABLE        = 16'h40F5;
    localparam logic [15:0] R62_DSP_RUN           = 16'h40F6;
    localparam logic [15:0] R63_DSP_SLEW          = 16'h40F7;
    localparam logic [15:0] R64_SERIAL_RATE       = 16'h40F8;
    localparam logic [15:0] R65_CLOCK_ENABLE_0    = 16'h40F9;
    localparam logic [15:0] R66_CLOCK_ENABLE_1    = 16'h40FA;

    localparam logic [7:0] R0_COREN  = 8'h01;   // core clock enable
    localparam logic [7:0] R1_LOCKED = 8'h02;   // R1's last byte: PLL locked

    // Output volumes, 1 dB a step from -57 dB at 0: the line out at 0 dB
    // for an amplifier's input, the headphones 10 dB down.
    localparam logic [5:0] VOL_0DB      = 6'd57;
    localparam logic [5:0] VOL_MINUS_10 = 6'd47;

    // ── The PLL ──────────────────────────────────────────────
    // R1 multiplies MCLK by R + N/M to the core clock. The fraction is
    // reduced to lowest terms; an exact multiple runs the PLL in integer
    // mode, with N and M ignored.
    localparam longint CORE_HZ = 49_152_000;
    localparam longint MCLK    = longint'(MCLK_HZ);

    function automatic longint gcd(longint a, longint b);
        longint t;
        for (int i = 0; i < 64; i++) begin
            if (b != 0) begin
                t = a % b;
                a = b;
                b = t;
            end
        end
        return a;
    endfunction

    localparam longint PLL_R    = CORE_HZ / MCLK;
    localparam longint PLL_REM  = CORE_HZ - PLL_R * MCLK;
    localparam longint PLL_DIV  = (PLL_REM == 0) ? MCLK : gcd(PLL_REM, MCLK);
    localparam longint PLL_N    = PLL_REM / PLL_DIV;
    localparam longint PLL_M    = MCLK / PLL_DIV;
    localparam logic   PLL_FRAC = (PLL_REM != 0);

    build_check #(
        .COND (MCLK_HZ >= 8_000_000 && MCLK_HZ <= 27_000_000),
        .MSG  ("nexysvideo_codec_setup: MCLK_HZ outside the codec PLL's 8-27 MHz input")
    ) u_check_mclk ();

    build_check #(
        .COND (PLL_R >= 2 && PLL_R <= 8 && PLL_M <= 65535),
        .MSG  ("nexysvideo_codec_setup: MCLK_HZ gives no R + N/M the codec PLL can hold")
    ) u_check_pll ();

    // R1's six bytes, M and N most significant first, then R, the input
    // divider (1) and the mode, then the enable.
    function automatic logic [7:0] pll_byte(logic [2:0] idx, logic enable);
        case (idx)
            3'd0:    return 8'(PLL_M >> 8);
            3'd1:    return 8'(PLL_M);
            3'd2:    return 8'(PLL_N >> 8);
            3'd3:    return 8'(PLL_N);
            3'd4:    return {1'b0, 4'(PLL_R), 2'b00, PLL_FRAC};
            default: return {7'd0, enable};
        endcase
    endfunction

    // ── Steps ────────────────────────────────────────────────
    typedef enum logic [2:0] {
        OP_WRITE,           // one byte to one register
        OP_WRITE_PLL,       // R1's six bytes; value is the PLL enable
        OP_IF_RUNNING,      // read R0: unless the core clock runs, skip `value` steps
        OP_WAIT_LOCK,       // read R1 until the PLL locks
        OP_WAIT_MS,         // wait `value` milliseconds
        OP_DONE
    } op_e;

    typedef struct packed {
        op_e         op;
        logic [15:0] addr;          // the register
        logic [7:0]  value;
    } step_t;

    localparam int STEP_W = 6;

    logic [STEP_W-1:0] step_idx_q;
    step_t             step;

    always_comb begin
        case (step_idx_q)
            // A codec still running is muted first. Each output register
            // holds its volume, its mute (bit 1, 0 mutes) and its mode.
            6'd0:  step = {OP_IF_RUNNING, R0_CLOCK_CONTROL,      8'd4};
            6'd1:  step = {OP_WRITE,      R29_HP_LEFT,           {VOL_MINUS_10, 1'b0, 1'b1}};  // HPEN
            6'd2:  step = {OP_WRITE,      R30_HP_RIGHT,          {VOL_MINUS_10, 1'b0, 1'b1}};  // HPMODE
            6'd3:  step = {OP_WRITE,      R31_LINE_LEFT,         {VOL_0DB,      1'b0, 1'b0}};  // line mode
            6'd4:  step = {OP_WRITE,      R32_LINE_RIGHT,        {VOL_0DB,      1'b0, 1'b0}};

            // The core clock from the PLL: stopped while the PLL is set
            // and started, then started itself once the PLL has locked.
            6'd5:  step = {OP_WRITE,      R0_CLOCK_CONTROL,      8'h0E};  // PLL source, 1024 fs, core off
            6'd6:  step = {OP_WRITE_PLL,  R1_PLL_CONTROL,        8'd0};
            6'd7:  step = {OP_WRITE_PLL,  R1_PLL_CONTROL,        8'd1};
            6'd8:  step = {OP_WAIT_LOCK,  R1_PLL_CONTROL,        8'd0};
            6'd9:  step = {OP_WRITE,      R0_CLOCK_CONTROL,      8'h0F};  // core on

            // Every output muted, whatever the codec held before.
            6'd10: step = {OP_WRITE,      R29_HP_LEFT,           {VOL_MINUS_10, 1'b0, 1'b1}};
            6'd11: step = {OP_WRITE,      R30_HP_RIGHT,          {VOL_MINUS_10, 1'b0, 1'b1}};
            6'd12: step = {OP_WRITE,      R31_LINE_LEFT,         {VOL_0DB,      1'b0, 1'b0}};
            6'd13: step = {OP_WRITE,      R32_LINE_RIGHT,        {VOL_0DB,      1'b0, 1'b0}};

            // The serial port: I2S at 48 kHz, 64 BCLKs a frame, with the
            // codec driving BCLK and LRCLK itself (the datasheet calls
            // that its serial port's master mode; on I2C the codec is
            // still the slave). Its input goes straight to the DACs, the
            // DSP off. The dejitter absorbs serial clocks that arrive from
            // outside the codec, and has nothing to absorb from its own,
            // so it is bypassed: a subsystem restarted while running can
            // leave it passing no audio. Were the FPGA to drive BCLK and
            // LRCLK, the window would have to go back to 3.
            6'd14: step = {OP_WRITE,      R15_SERIAL_PORT_0,     8'h01};  // MS: the codec drives BCLK, LRCLK
            6'd15: step = {OP_WRITE,      R16_SERIAL_PORT_1,     8'h00};  // 64 BCLKs, 1-BCLK delay
            6'd16: step = {OP_WRITE,      R17_CONVERTER_0,       8'h00};  // converters at fs
            6'd17: step = {OP_WRITE,      R64_SERIAL_RATE,       8'h00};  // serial port at fs
            6'd18: step = {OP_WRITE,      R60_SERIAL_PINS,       8'h00};  // serial pins, not GPIO
            6'd19: step = {OP_WRITE,      R58_SERIAL_IN_ROUTE,   8'h01};  // serial L0/R0 to the DACs
            6'd20: step = {OP_WRITE,      R61_DSP_ENABLE,        8'h00};
            6'd21: step = {OP_WRITE,      R62_DSP_RUN,           8'h00};
            6'd22: step = {OP_WRITE,      R63_DSP_SLEW,          8'h00};  // volume slew by the codec
            6'd23: step = {OP_WRITE,      R67_DEJITTER,          8'h00};
            6'd24: step = {OP_WRITE,      R65_CLOCK_ENABLE_0,    8'h7F};
            6'd25: step = {OP_WRITE,      R66_CLOCK_ENABLE_1,    8'h03};  // converter and serial clocks

            // DACs at 0 dB into mixers 3 and 4, which feed the
            // headphones and, through mixers 5 and 6 at 0 dB on their one
            // input each, the line out. The record path and the mono
            // output stay out of it.
            6'd26: step = {OP_WRITE,      R37_DAC_VOLUME_LEFT,   8'h00};
            6'd27: step = {OP_WRITE,      R38_DAC_VOLUME_RIGHT,  8'h00};
            6'd28: step = {OP_WRITE,      R36_DAC_CONTROL_0,     8'h03};  // both DACs on
            6'd29: step = {OP_WRITE,      R22_MIXER3_LEFT_0,     8'h21};  // left DAC, enabled
            6'd30: step = {OP_WRITE,      R23_MIXER3_LEFT_1,     8'h00};
            6'd31: step = {OP_WRITE,      R24_MIXER4_RIGHT_0,    8'h41};  // right DAC, enabled
            6'd32: step = {OP_WRITE,      R25_MIXER4_RIGHT_1,    8'h00};
            6'd33: step = {OP_WRITE,      R26_MIXER5_LINE_LEFT,  8'h05};  // mixer 3 at 0 dB, enabled
            6'd34: step = {OP_WRITE,      R27_MIXER6_LINE_RIGHT, 8'h11};  // mixer 4 at 0 dB, enabled
            6'd35: step = {OP_WRITE,      R28_MIXER7_MONO,       8'h00};
            6'd36: step = {OP_WRITE,      R33_MONO,              8'h00};  // muted
            6'd37: step = {OP_WRITE,      R34_POP_CLICK,         8'h00};  // suppression on, 21 ms slew
            6'd38: step = {OP_WRITE,      R35_PLAYBACK_POWER,    8'h03};  // both playback channels

            // The headphone outputs need 4 ms of precharge once powered
            // before they are unmuted; the line outputs 2 to 3 ms.
            6'd39: step = {OP_WAIT_MS,    16'h0000,              8'd10};
            6'd40: step = {OP_WRITE,      R29_HP_LEFT,           {VOL_MINUS_10, 1'b1, 1'b1}};
            6'd41: step = {OP_WRITE,      R30_HP_RIGHT,          {VOL_MINUS_10, 1'b1, 1'b1}};
            6'd42: step = {OP_WRITE,      R31_LINE_LEFT,         {VOL_0DB,      1'b1, 1'b0}};
            6'd43: step = {OP_WRITE,      R32_LINE_RIGHT,        {VOL_0DB,      1'b1, 1'b0}};
            default: step = {OP_DONE,     16'h0000,              8'd0};
        endcase
    end

    // ── One step's transaction ───────────────────────────────
    // A write is the address, the register's two address bytes and its
    // data; a read is the same address bytes, then a repeated START, the
    // address for reading and the data, the last byte answered with
    // NACK. Both read steps need only the last byte read: R0 has one,
    // and R1 keeps its lock bit in the last of its six.
    logic       step_reads;
    logic [2:0] data_bytes;
    logic [3:0] last_pos;

    assign step_reads = (step.op == OP_IF_RUNNING) || (step.op == OP_WAIT_LOCK);
    assign data_bytes = (step.op == OP_WRITE_PLL || step.op == OP_WAIT_LOCK) ? 3'd6 : 3'd1;
    assign last_pos   = step_reads ? 4'd3 + 4'(data_bytes) : 4'd2 + 4'(data_bytes);

    logic [3:0] pos_q;          // the byte of the transaction

    always_comb begin
        o_cmd_start = 1'b0;
        o_cmd_stop  = (pos_q == last_pos);
        o_cmd_read  = 1'b0;
        o_cmd_nack  = 1'b0;
        o_cmd_wdata = 8'h00;
        case (pos_q)
            4'd0: begin
                o_cmd_start = 1'b1;
                o_cmd_wdata = {CODEC_ADDR, 1'b0};
            end
            4'd1: o_cmd_wdata = step.addr[15:8];
            4'd2: o_cmd_wdata = step.addr[7:0];
            default: begin
                if (!step_reads) begin
                    o_cmd_wdata = (step.op == OP_WRITE_PLL) ? pll_byte(3'(pos_q - 4'd3), step.value[0])
                                                            : step.value;
                end else if (pos_q == 4'd3) begin
                    o_cmd_start = 1'b1;
                    o_cmd_wdata = {CODEC_ADDR, 1'b1};
                end else begin
                    o_cmd_read = 1'b1;
                    o_cmd_nack = o_cmd_stop;
                end
            end
        endcase
    end

    // ── Sequencing ───────────────────────────────────────────
    typedef enum logic [2:0] {
        S_STEP,         // start the step at step_idx_q
        S_SEND,         // a byte of its transaction offered to the I2C master
        S_ANSWER,       // waiting for the I2C master's response
        S_WAIT,         // an OP_WAIT_MS step
        S_DONE,
        S_FAULT
    } state_e;

    localparam logic [1:0] FAULT_NACK = 2'd1;
    localparam logic [1:0] FAULT_BUS  = 2'd2;
    localparam logic [1:0] FAULT_LOCK = 2'd3;

    localparam int CYCLES_PER_MS   = CLK_HZ / 1000;
    localparam int LOCK_TIMEOUT_MS = 20;

    state_e     state_q;
    logic [7:0] fault_code_q;
    logic [$clog2(CYCLES_PER_MS)-1:0] tick_q;   // cycles into the millisecond
    logic [7:0] ms_q;                           // milliseconds into the wait

    logic ms_end;
    assign ms_end = (tick_q == ($clog2(CYCLES_PER_MS))'(CYCLES_PER_MS - 1));

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q      <= S_STEP;
            step_idx_q   <= '0;
            pos_q        <= 4'd0;
            fault_code_q <= 8'd0;
            tick_q       <= '0;
            ms_q         <= 8'd0;
        end else begin
            // The millisecond count runs from the start of each step, and
            // OP_WAIT_LOCK times its whole poll on it.
            if (state_q == S_STEP) begin
                tick_q <= '0;
                ms_q   <= 8'd0;
            end else if (ms_end) begin
                tick_q <= '0;
                if (ms_q != 8'hFF)
                    ms_q <= ms_q + 8'd1;
            end else begin
                tick_q <= tick_q + 1'b1;
            end

            case (state_q)
                S_STEP: begin
                    pos_q <= 4'd0;
                    case (step.op)
                        OP_DONE:    state_q <= S_DONE;
                        OP_WAIT_MS: state_q <= S_WAIT;
                        default:    state_q <= S_SEND;
                    endcase
                end

                S_SEND: begin
                    if (i_cmd_ready)
                        state_q <= S_ANSWER;
                end

                S_ANSWER: begin
                    if (i_rsp_valid) begin
                        if (i_rsp_error) begin
                            fault_code_q <= {FAULT_BUS, step_idx_q};
                            state_q      <= S_FAULT;
                        end else if (!o_cmd_read && i_rsp_nack) begin
                            fault_code_q <= {FAULT_NACK, step_idx_q};
                            state_q      <= S_FAULT;
                        end else if (pos_q != last_pos) begin
                            pos_q   <= pos_q + 4'd1;
                            state_q <= S_SEND;
                        end else begin
                            state_q <= S_STEP;
                            case (step.op)
                                OP_IF_RUNNING: begin
                                    if ((i_rsp_rdata & R0_COREN) != 0)
                                        step_idx_q <= step_idx_q + 1'b1;
                                    else
                                        step_idx_q <= step_idx_q + 1'b1 + STEP_W'(step.value);
                                end
                                OP_WAIT_LOCK: begin
                                    if ((i_rsp_rdata & R1_LOCKED) != 0) begin
                                        step_idx_q <= step_idx_q + 1'b1;
                                    end else if (ms_q >= 8'(LOCK_TIMEOUT_MS)) begin
                                        fault_code_q <= {FAULT_LOCK, step_idx_q};
                                        state_q      <= S_FAULT;
                                    end else begin
                                        // Poll again, without restarting the timeout.
                                        pos_q   <= 4'd0;
                                        state_q <= S_SEND;
                                    end
                                end
                                default: step_idx_q <= step_idx_q + 1'b1;
                            endcase
                        end
                    end
                end

                S_WAIT: begin
                    if (ms_q >= step.value) begin
                        step_idx_q <= step_idx_q + 1'b1;
                        state_q    <= S_STEP;
                    end
                end

                default: ;      // S_DONE, S_FAULT: held until reset
            endcase
        end
    end

    assign o_cmd_valid  = (state_q == S_SEND);
    assign o_busy       = (state_q != S_DONE) && (state_q != S_FAULT);
    assign o_fault      = (state_q == S_FAULT);
    assign o_fault_code = fault_code_q;

endmodule
