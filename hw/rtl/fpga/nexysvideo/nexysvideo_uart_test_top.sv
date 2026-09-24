// Nexys Video UART test top: console, clocking and reset bring-up
//
// Proves the clocking and reset every Nexys top shares
// (nexysvideo_clk_rst) and the console port: a bare serial shifter
// prints a banner after every reset, then echoes each byte it receives.
// No CPU, bus or memory. The console is 115200 8N1 on the UART USB port.
//
// Each LED reports one piece on its own, so one bitstream localizes a
// failure:
//   led[0]  MMCM locked
//   led[1]  reset released
//   led[2]  heartbeat: toggles every CPU_HZ / 2 cycles, a 1 Hz blink
//           when the CPU clock is right. It ignores reset, so a stuck
//           reset still shows whether the clock runs.
//   led[3]  toggles on every byte received
//   led[4]  a received frame ended in a low stop bit (wrong baud, or a
//           break); held until reset

module nexysvideo_uart_test_top (
    input  logic       clk,          // 100 MHz oscillator
    input  logic       cpu_resetn,   // CPU RESET button, low while pressed
    output logic [7:0] led,
    output logic       uart_rx_out,  // FPGA TX, to the host's RX
    input  logic       uart_tx_in    // FPGA RX, from the host's TX
);
    localparam longint CPU_HZ = 37_500_000;
    localparam longint BAUD   = 115_200;

    logic cpu_clk;
    logic locked;
    logic rst;

    nexysvideo_clk_rst #(
        .CPU_HZ (CPU_HZ)
    ) u_clk_rst (
        .i_clk_100mhz (clk),
        .i_cpu_resetn (cpu_resetn),
        .o_clk        (cpu_clk),
        .o_locked     (locked),
        .o_rst        (rst)
    );

    // ── Bit timing ─────────────────────────────────────────────
    // A bit lasts BIT_CYCLES CPU clocks, rounded to the nearest. A
    // receiver tolerates a few percent of error across a frame; the
    // build refuses more than 1%.
    localparam int     BIT_CYCLES = int'((CPU_HZ + BAUD / 2) / BAUD);
    localparam longint BIT_ERR    = longint'(BIT_CYCLES) * BAUD - CPU_HZ;

    build_check #(
        .COND (BIT_ERR * 100 <= CPU_HZ && -BIT_ERR * 100 <= CPU_HZ),
        .MSG  ("uart test: BAUD is more than 1% from a whole number of CPU_HZ cycles")
    ) u_check_baud ();

    localparam int                TICK_W    = $clog2(BIT_CYCLES);
    localparam logic [TICK_W-1:0] TICK_LAST = TICK_W'(BIT_CYCLES - 1);
    localparam logic [TICK_W-1:0] TICK_HALF = TICK_W'(BIT_CYCLES / 2 - 1);

    // ── TX ─────────────────────────────────────────────────────
    // A frame is a start bit, eight data bits LSB first and a stop bit.
    // tx_frame_q holds what is left of it, next bit at [0], and shifts
    // in ones, so the line idles high once the frame is out. It starts
    // high from configuration too: the CPU clock only runs once the MMCM
    // locks, and a line held low until reset takes effect would reach the
    // host as a break.
    logic              tx_load;
    logic [7:0]        tx_data;
    logic              tx_idle;
    /* verilator lint_off PROCASSINIT */
    logic [9:0]        tx_frame_q = '1;
    /* verilator lint_on PROCASSINIT */
    logic [3:0]        tx_bits_q;    // bits of the frame still to send
    logic [TICK_W-1:0] tx_tick_q;    // cycles into the current bit

    assign tx_idle = (tx_bits_q == 4'd0);

    always_ff @(posedge cpu_clk) begin
        if (rst) begin
            tx_frame_q <= '1;
            tx_bits_q  <= 4'd0;
            tx_tick_q  <= '0;
        end else if (tx_load) begin
            tx_frame_q <= {1'b1, tx_data, 1'b0};
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

    assign uart_rx_out = tx_frame_q[0];

    // ── RX ─────────────────────────────────────────────────────
    // The line is sampled mid-bit: half a bit after the start edge, then
    // once per bit. A start bit that is high again at its middle was a
    // glitch, and the receiver goes back to waiting. A frame starts on a
    // falling edge rather than a low level, so after a low stop bit or a
    // break the receiver waits for the line to return high.
    (* ASYNC_REG = "TRUE" *) logic rx_sync1, rx_sync2;
    logic rx_prev;
    always_ff @(posedge cpu_clk) begin
        rx_sync1 <= uart_tx_in;
        rx_sync2 <= rx_sync1;
        rx_prev  <= rx_sync2;
    end

    logic [7:0]        rx_data_q;     // shifts in LSB first
    logic [3:0]        rx_bits_q;     // samples still to take; 0 = idle
    logic [TICK_W-1:0] rx_tick_q;     // cycles to the next sample
    logic              rx_valid_q;    // rx_data_q is a new byte
    logic              rx_toggle_q;
    logic              rx_frame_err_q;

    always_ff @(posedge cpu_clk) begin
        rx_valid_q <= 1'b0;
        if (rst) begin
            rx_bits_q      <= 4'd0;
            rx_tick_q      <= '0;
            rx_toggle_q    <= 1'b0;
            rx_frame_err_q <= 1'b0;
        end else if (rx_bits_q == 4'd0) begin
            if (rx_prev && !rx_sync2) begin
                rx_bits_q <= 4'd10;
                rx_tick_q <= TICK_HALF;
            end
        end else if (rx_tick_q != '0) begin
            rx_tick_q <= rx_tick_q - TICK_W'(1);
        end else begin
            rx_tick_q <= TICK_LAST;
            rx_bits_q <= rx_bits_q - 4'd1;
            // The start bit shifts in too; the eight data bits after it
            // push it out.
            if (rx_bits_q != 4'd1)
                rx_data_q <= {rx_sync2, rx_data_q[7:1]};
            if (rx_bits_q == 4'd10 && rx_sync2)
                rx_bits_q <= 4'd0;
            if (rx_bits_q == 4'd1) begin
                rx_valid_q     <= rx_sync2;
                rx_toggle_q    <= rx_toggle_q ^ rx_sync2;
                rx_frame_err_q <= rx_frame_err_q || !rx_sync2;
            end
        end
    end

    // ── Banner, then echo ──────────────────────────────────────
    // A byte that arrives while the banner is going out waits in echo_q;
    // a second one replaces it.
    localparam BANNER = "\r\nPenumbra Nexys Video UART test\r\n";
    localparam int BANNER_LEN = $bits(BANNER) / 8;
    localparam int BANNER_W   = $clog2(BANNER_LEN + 1);

    logic [BANNER_W-1:0] banner_idx_q;    // next banner character
    logic                banner_done;
    logic [7:0]          banner_char;
    logic [7:0]          echo_q;
    logic                echo_pending_q;

    assign banner_done = (banner_idx_q == BANNER_W'(BANNER_LEN));
    assign banner_char = BANNER[8 * (BANNER_LEN - 1 - int'(banner_idx_q)) +: 8];
    assign tx_load     = tx_idle && (!banner_done || echo_pending_q);
    assign tx_data     = banner_done ? echo_q : banner_char;

    always_ff @(posedge cpu_clk) begin
        if (rst) begin
            banner_idx_q   <= '0;
            echo_pending_q <= 1'b0;
        end else begin
            if (tx_load && !banner_done)
                banner_idx_q <= banner_idx_q + BANNER_W'(1);
            if (rx_valid_q) begin
                echo_q         <= rx_data_q;
                echo_pending_q <= 1'b1;
            end else if (tx_load && banner_done) begin
                echo_pending_q <= 1'b0;
            end
        end
    end

    // ── Heartbeat ──────────────────────────────────────────────
    localparam int                BEAT_HALF = int'(CPU_HZ / 2);
    localparam int                BEAT_W    = $clog2(BEAT_HALF);
    localparam logic [BEAT_W-1:0] BEAT_LAST = BEAT_W'(BEAT_HALF - 1);

    logic [BEAT_W-1:0] beat_cnt_q;
    logic              beat_q;

    always_ff @(posedge cpu_clk) begin
        if (beat_cnt_q == BEAT_LAST) begin
            beat_cnt_q <= '0;
            beat_q     <= !beat_q;
        end else begin
            beat_cnt_q <= beat_cnt_q + BEAT_W'(1);
        end
    end

    assign led = {3'b000, rx_frame_err_q, rx_toggle_q, beat_q, !rst, locked};

endmodule
