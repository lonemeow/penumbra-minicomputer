// Penumbra UART — NS16550A-compatible with real serial I/O
//
// Drop-in replacement for sim_uart on real hardware. Same bus-side
// register interface (address decode, read/write protocol, interrupt
// logic), with actual baud rate generation and serial TX/RX pins
// instead of testbench handshake signals.
//
// Acts as NS16450 when FCR[0]=0, NS16550A with FIFOs when FCR[0]=1.
// NetBSD com(4) detects the FIFO by writing FCR[0]=1 and reading the
// IIR[7:6] mode bits back (00 = 16450, 11 = 16550A FIFO).
//
// Register map (word offsets from base, word-strided):
//   0x000  RBR/THR (DLAB=0)  or  DLL (DLAB=1)
//   0x004  IER     (DLAB=0)  or  DLM (DLAB=1)
//   0x008  IIR (read)  /  FCR (write)
//   0x00C  LCR  (DLAB bit = bit 7)
//   0x010  MCR  (OUT2 = bit 3 = master IRQ enable)
//   0x014  LSR  (bit 0=DR, bit 4=BI, bit 5=THRE, bit 6=TEMT)
//   0x018  MSR  (CTS+DSR hardwired asserted)
//   0x01C  SCR  (scratch register)
//
// FCR layout (write-only at 0x008):
//   bit 0     FIFO enable    (latched)
//   bit 1     RX FIFO reset  (1-cycle pulse)
//   bit 2     TX FIFO reset  (1-cycle pulse)
//   bits 7:6  RX trigger     (00=1, 01=4, 10=8, 11=14 bytes)
//
// IIR layout (read at 0x008):
//   bits 7:6  FIFO status    (00 = 16450, 11 = 16550A FIFOs active)
//   bits 3:0  interrupt id   0001=none, 0010=THRE, 0100=RX-above-trigger,
//                            1100=character timeout (FIFO mode only)
//
// Character timeout: in FIFO mode, when the RX FIFO is non-empty and
// the line has been idle for >4 character-times, an RX interrupt
// fires even if the trigger threshold hasn't been reached.  This
// makes partial bursts (e.g. paste-into-console) deliver promptly.
//
// Baud rate generation is two-stage:
//
//   Stage 1: a fractional accumulator divides the actual system
//            clock (CLK_FREQ) down to a fixed REF_FREQ tick stream.
//            Default REF_FREQ is 1.8432 MHz — the canonical 16450
//            crystal — so software sees a standard PC UART regardless
//            of what CLK_FREQ actually is on this board.
//
//   Stage 2: the existing software-programmed divisor {DLM, DLL}
//            divides the REF_FREQ ticks by D, giving a 16x baud
//            clock at REF_FREQ / D Hz.  Standard NS16450 formula:
//            baud = REF_FREQ / (16 * D).
//
// CLK_FREQ is a hardware-implementation detail — software never
// sees it.  Drivers configure the UART exactly as if it had a
// 1.8432 MHz crystal, and divisor=1 produces 115200 baud on every
// board variant.
//
// Compatible with NetBSD com(4): reg-shift=2, reg-io-width=4.

// keep_hierarchy: keep this module's cells placed as a cluster.
// Without it, after the 16550A FIFO promotion (~256 storage flops
// plus pointer/decode/IIR/timeout logic) the placer scattered UART
// cells through slices the CPU's MAR→ESR critical path was relying
// on, dropping fmax from ~28 MHz to 26.4 MHz on the ECP5-85F sg6.
// Matches the same treatment on sdram_ctrl / sdram_cdc.
(* keep_hierarchy = "yes" *)
module uart
    import penumbra_pkg::*;
#(
    parameter int CLK_FREQ   = 25_000_000,
    parameter int REF_FREQ   =  1_843_200, // advertised 16450 crystal
    parameter int FIFO_DEPTH = 16          // 16550A standard depth
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface (same as sim_uart) ─────────────
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,   /* only bits [4:2] used (8 word regs) */
    input  logic [31:0] i_wdata,  /* only bits [7:0] used (8-bit regs)  */
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Serial pins ─────────────────────────────────────────
    output logic        o_tx,
    input  logic        i_rx,

    // ── Interrupt output ────────────────────────────────────
    output logic        o_irq
);

    // Default divisor = 1: with REF_FREQ = 1.8432 MHz, this gives
    // 115200 baud out of reset — the rate the boot ROM and early
    // kernel console expect, with no software configuration needed.
    localparam int DEFAULT_DIVISOR = 1;

    // ── Register select from word-aligned address ───────────
    logic [2:0] reg_sel;
    assign reg_sel = i_addr[4:2];

    // ══════════════════════════════════════════════════════════
    // Internal registers
    // ══════════════════════════════════════════════════════════
    logic [7:0] rbr;
    logic [7:0] ier;
    logic [7:0] lcr;
    logic [7:0] mcr;
    logic [7:0] scr;
    logic [7:0] dll, dlm;
    logic       rx_ready;
    logic       rx_break;   // LSR.BI sticky bit; clears on LSR read
    logic       thre_int;

    logic dlab;
    assign dlab = lcr[7];

    // ══════════════════════════════════════════════════════════
    // FCR — FIFO Control Register (write-only at 0x008)
    // ══════════════════════════════════════════════════════════
    // bit 0     latched: FIFO enable
    // bits 7:6  latched: RX trigger level select
    // bits 1,2  write-1-pulse: RX/TX FIFO reset (not stored)
    /* verilator lint_off UNUSEDSIGNAL */
    logic [7:0] fcr;       // fcr[5:1] always 0; kept full-width for readback ergonomics
    /* verilator lint_on UNUSEDSIGNAL */
    logic       fcr_fifo_enable;
    logic [1:0] fcr_trigger;
    logic       fcr_write;
    logic       rx_fifo_flush;
    logic       tx_fifo_flush;

    assign fcr_fifo_enable = fcr[0];
    assign fcr_trigger     = fcr[7:6];
    assign fcr_write       = i_we && (reg_sel == 3'd2);

    // The FIFO reset bits self-clear: pulse the flush signal during
    // the FCR write cycle, never stored back into fcr[1] / fcr[2].
    // Both bits also flush on global reset.
    assign rx_fifo_flush = i_rst || (fcr_write && i_wdata[1]);
    assign tx_fifo_flush = i_rst || (fcr_write && i_wdata[2]);

    // RX trigger threshold decode (FCR[7:6]).  Hardware comparator
    // is just a 2-bit→4-value LUT, then magnitude-compare against
    // the live FIFO level.
    logic [4:0] trigger_threshold;     // up to FIFO_DEPTH (16)
    always_comb begin
        unique case (fcr_trigger)
            2'b00: trigger_threshold = 5'd1;
            2'b01: trigger_threshold = 5'd4;
            2'b10: trigger_threshold = 5'd8;
            2'b11: trigger_threshold = 5'd14;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // RX / TX FIFOs (spi_fifo, parameterized ring buffers)
    // ══════════════════════════════════════════════════════════
    logic                          rx_fifo_push, rx_fifo_pop;
    logic [7:0]                    rx_fifo_rdata;
    logic                          rx_fifo_full, rx_fifo_empty;
    logic [$clog2(FIFO_DEPTH+1)-1:0] rx_fifo_level;

    logic                          tx_fifo_push, tx_fifo_pop;
    logic [7:0]                    tx_fifo_wdata;
    logic [7:0]                    tx_fifo_rdata;
    logic                          tx_fifo_full, tx_fifo_empty;
    logic [$clog2(FIFO_DEPTH+1)-1:0] tx_fifo_level;
    /* verilator lint_off UNUSEDSIGNAL */
    logic                          rx_fifo_full_unused;
    logic [$clog2(FIFO_DEPTH+1)-1:0] tx_fifo_level_unused;
    /* verilator lint_on UNUSEDSIGNAL */

    // RX byte staged for FIFO push (see RX state machine below).
    logic [7:0] rx_push_data;

    spi_fifo #(.DEPTH(FIFO_DEPTH)) u_rx_fifo (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_wr    (rx_fifo_push),
        .i_wdata (rx_push_data),
        .i_rd    (rx_fifo_pop),
        .o_rdata (rx_fifo_rdata),
        .o_full  (rx_fifo_full),
        .o_empty (rx_fifo_empty),
        .o_level (rx_fifo_level),
        .i_flush (rx_fifo_flush)
    );

    spi_fifo #(.DEPTH(FIFO_DEPTH)) u_tx_fifo (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_wr    (tx_fifo_push),
        .i_wdata (tx_fifo_wdata),
        .i_rd    (tx_fifo_pop),
        .o_rdata (tx_fifo_rdata),
        .o_full  (tx_fifo_full),
        .o_empty (tx_fifo_empty),
        .o_level (tx_fifo_level),
        .i_flush (tx_fifo_flush)
    );

    assign rx_fifo_full_unused  = rx_fifo_full;
    assign tx_fifo_level_unused = tx_fifo_level;

    // ══════════════════════════════════════════════════════════
    // Baud rate generator — Stage 1: fractional reference clock
    // ══════════════════════════════════════════════════════════
    // Synthesize a fixed REF_FREQ tick stream from CLK_FREQ using
    // a DDA-style fractional accumulator.  Each system clock the
    // accumulator advances by REF_FREQ; whenever it would equal or
    // exceed CLK_FREQ, a single-cycle ref_tick fires and CLK_FREQ
    // is subtracted (preserving the fractional remainder so there
    // is no long-term drift).
    //
    // Average ref_tick rate = REF_FREQ Hz, exact within accumulator
    // precision — orders of magnitude tighter than UART tolerance.
    //
    // Accumulator must hold values up to (CLK_FREQ - 1) + REF_FREQ.
    localparam int ACC_WIDTH = $clog2(CLK_FREQ + REF_FREQ);

    logic [ACC_WIDTH-1:0] ref_acc;
    logic                 ref_tick;

    /*
     * `tmp' is declared `automatic' so its lifetime matches the
     * always_ff invocation; without that, `int tmp = expr' inside
     * an always_ff is treated as a one-time static init and the
     * accumulator never ticks under Verilator.  Width is ACC_WIDTH+1
     * to absorb the addition's carry-out cleanly.
     */
    always_ff @(posedge i_clk) begin
        automatic logic [ACC_WIDTH:0] tmp;
        if (i_rst) begin
            ref_acc  <= '0;
            ref_tick <= 1'b0;
        end else begin
            tmp = {1'b0, ref_acc} + (ACC_WIDTH+1)'(REF_FREQ);
            if (tmp >= (ACC_WIDTH+1)'(CLK_FREQ)) begin
                ref_acc  <= tmp[ACC_WIDTH-1:0]
                          - ACC_WIDTH'(CLK_FREQ);
                ref_tick <= 1'b1;
            end else begin
                ref_acc  <= tmp[ACC_WIDTH-1:0];
                ref_tick <= 1'b0;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Baud rate generator — Stage 2: software-programmed divisor
    // ══════════════════════════════════════════════════════════
    // Standard NS16450: divide REF_FREQ by {DLM, DLL} to get the
    // 16x baud clock.  baud = REF_FREQ / (16 * divisor).
    // Divisor of 0 is treated as 1 to avoid divide-by-zero lockup.
    logic [15:0] divisor;
    assign divisor = ({dlm, dll} == 16'd0) ? 16'd1 : {dlm, dll};

    logic [15:0] baud_cnt;
    logic        baud16x_tick;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            baud_cnt     <= 16'd0;
            baud16x_tick <= 1'b0;
        end else if (ref_tick) begin
            if (baud_cnt >= divisor - 16'd1) begin
                baud_cnt     <= 16'd0;
                baud16x_tick <= 1'b1;
            end else begin
                baud_cnt     <= baud_cnt + 16'd1;
                baud16x_tick <= 1'b0;
            end
        end else begin
            baud16x_tick <= 1'b0;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Bus protocol — 1-cycle read latency (same as sim_uart)
    // ══════════════════════════════════════════════════════════
    logic access_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_pending <= 1'b0;
        else if (i_re && !access_pending)
            access_pending <= 1'b1;
        else
            access_pending <= 1'b0;
    end

    assign o_busy = i_re && !access_pending;

    logic read_first;
    assign read_first = i_re && !access_pending;

    // ══════════════════════════════════════════════════════════
    // TX shift register
    // ══════════════════════════════════════════════════════════
    logic [9:0]  tx_shift;     // {stop, data[7:0], start}
    logic [3:0]  tx_bit_idx;   // 0-9: current bit position
    logic [3:0]  tx_sample_cnt;// 0-15: 16x sub-bit counter
    logic        tx_active;    // shifting in progress

    // THR write detection
    logic thr_write;
    assign thr_write = i_we && !dlab && (reg_sel == 3'd0);

    // FIFO mode: THR writes push into tx_fifo (when not full).  Bypass
    // mode: THR write loads the shift register directly when idle.
    assign tx_fifo_push  = fcr_fifo_enable && thr_write && !tx_fifo_full;
    assign tx_fifo_wdata = i_wdata[7:0];

    // tx_load: pulse to load shift register; tx_load_data: source byte.
    logic       tx_load;
    logic [7:0] tx_load_data;
    always_comb begin
        if (fcr_fifo_enable) begin
            tx_load      = !tx_active && !tx_fifo_empty;
            tx_load_data = tx_fifo_rdata;
        end else begin
            tx_load      = !tx_active && thr_write;
            tx_load_data = i_wdata[7:0];
        end
    end

    // Drain the FIFO head as the shifter picks it up.
    assign tx_fifo_pop = fcr_fifo_enable && tx_load;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_shift      <= 10'h3FF; // all ones = idle
            tx_bit_idx    <= 4'd0;
            tx_sample_cnt <= 4'd0;
            tx_active     <= 1'b0;
            o_tx          <= 1'b1;    // idle high
        end else if (tx_load) begin
            // Load shift register: {stop=1, data[7:0], start=0}
            tx_shift      <= {1'b1, tx_load_data, 1'b0};
            tx_bit_idx    <= 4'd0;
            tx_sample_cnt <= 4'd0;
            tx_active     <= 1'b1;
            o_tx          <= 1'b0;    // start bit begins immediately
        end else if (tx_active && baud16x_tick) begin
            if (tx_sample_cnt == 4'd15) begin
                // One full bit period elapsed — advance to next bit
                tx_sample_cnt <= 4'd0;
                if (tx_bit_idx == 4'd9) begin
                    // All 10 bits sent (start + 8 data + stop)
                    tx_active <= 1'b0;
                    o_tx      <= 1'b1; // idle
                end else begin
                    tx_bit_idx <= tx_bit_idx + 4'd1;
                    o_tx       <= tx_shift[tx_bit_idx + 4'd1];
                end
            end else begin
                tx_sample_cnt <= tx_sample_cnt + 4'd1;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // RX shift register
    // ══════════════════════════════════════════════════════════
    // 16x oversampling receiver: detects start bit falling edge,
    // samples data at mid-bit, delivers byte to RBR.

    // Synchronize i_rx to avoid metastability
    logic rx_sync1, rx_sync2;
    always_ff @(posedge i_clk) begin
        rx_sync1 <= i_rx;
        rx_sync2 <= rx_sync1;
    end

    // RBR read detection (clears rx_ready)
    logic rbr_read;
    assign rbr_read = read_first && !dlab && (reg_sel == 3'd0);

    // LSR read detection (clears the sticky rx_break / LSR.BI bit
    // per the standard 16450 "read-to-clear" convention).
    logic lsr_read;
    assign lsr_read = read_first && (reg_sel == 3'd5);

    logic [3:0]  rx_sample_cnt;
    logic [2:0]  rx_bit_idx;
    logic [7:0]  rx_shift;

    typedef enum logic [2:0] {IDLE, START, DATA, STOP} state_e_rx;
    state_e_rx rx_state;

    // ── RX → FIFO push & RBR-read → FIFO pop ──────────────────
    // Combinational pulse on the cycle the STOP bit finishes (the
    // same cycle that writes rbr/rx_ready in bypass mode).
    logic rx_byte_complete;
    assign rx_byte_complete =
        baud16x_tick && (rx_state == STOP) && (rx_sample_cnt == 4'd15);

    // FIFO mode: push freshly received byte (drop on overrun for now).
    // OE / per-byte error flags are not implemented in this iteration.
    assign rx_fifo_push = fcr_fifo_enable && rx_byte_complete && !rx_fifo_full;
    assign rx_push_data = rx_shift;

    // FIFO mode: RBR read advances the FIFO read pointer by one byte.
    assign rx_fifo_pop  = fcr_fifo_enable && rbr_read && !rx_fifo_empty;

    // RX state machine + RBR delivery
    //
    // BREAK detection: per the 16450 spec, LSR.BI is set when the
    // RX line is held SPACE (logic 0) for longer than one full
    // character frame.  We detect this in the STOP state: if the
    // stop bit samples as 0 and every data bit was also 0, the line
    // has been low for the entire frame — that's a BREAK.  A null
    // byte (the "break character") is delivered to RBR along the
    // normal path; software sees DR=1, BI=1, RBR=0x00.
    //
    // LSR-read-clears-BI is handled separately, outside the
    // baud-tick gate, so software can clear the sticky bit on any
    // cycle.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            rbr           <= 8'd0;
            rx_ready      <= 1'b0;
            rx_break      <= 1'b0;
            rx_state      <= IDLE;
        end else begin
            if (rbr_read)
                rx_ready <= 1'b0;
            if (lsr_read)
                rx_break <= 1'b0;
            if (baud16x_tick) begin
                case (rx_state)
                    IDLE: begin
                        if (!rx_sync2) begin
                            rx_state      <= START;
                            rx_sample_cnt <= 0;
                        end
                    end
                    START: begin
                        rx_sample_cnt <= rx_sample_cnt + 1;
                        if (rx_sample_cnt == 7) begin
                            if (!rx_sync2) begin
                                rx_state <= DATA;
                                rx_sample_cnt <= 0;
                                rx_bit_idx    <= 0;
                            end else begin
                                rx_state <= IDLE;
                            end
                        end
                    end
                    DATA: begin
                        rx_sample_cnt <= rx_sample_cnt + 1;
                        if (rx_sample_cnt == 15) begin
                            rx_sample_cnt        <= 0;
                            rx_shift[rx_bit_idx] <= rx_sync2;
                            rx_bit_idx           <= rx_bit_idx + 1;
                            if (rx_bit_idx == 7) begin
                                rx_state <= STOP;
                            end
                        end
                    end
                    STOP: begin
                        rx_sample_cnt <= rx_sample_cnt + 1;
                        if (rx_sample_cnt == 15) begin
                            rx_state <= IDLE;
                            rbr      <= rx_shift;
                            rx_ready <= 1'b1;
                            if (rx_shift == 8'h00 && !rx_sync2)
                                rx_break <= 1'b1;
                        end
                    end
                    default: rx_state <= IDLE;  /* unreachable; satisfies lint */
                endcase
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // LSR — Line Status Register
    // ══════════════════════════════════════════════════════════
    // bit 6: TEMT  — TX FIFO and shifter both empty
    // bit 5: THRE  — TX holding/FIFO empty (room to write more)
    // bit 4: BI    — break condition received (sticky, clears on LSR read)
    // bit 0: DR    — data ready (RBR has a received byte; FIFO non-empty)
    //
    // In bypass mode the FIFOs are unused: tx_fifo_empty stays 1
    // (nothing ever pushed) and rx_fifo_empty stays 1, so the
    // expressions below reduce to the original NS16450 semantics.
    logic dr_bit, thre_bit, temt_bit;
    assign dr_bit   = fcr_fifo_enable ? !rx_fifo_empty            : rx_ready;
    assign thre_bit = fcr_fifo_enable ?  tx_fifo_empty            : !tx_active;
    assign temt_bit = fcr_fifo_enable ? (tx_fifo_empty && !tx_active) : !tx_active;
    logic [7:0] lsr;
    assign lsr = {1'b0, temt_bit, thre_bit, rx_break, 3'b0, dr_bit};

    // ══════════════════════════════════════════════════════════
    // MSR — Modem Status Register (hardwired)
    // ══════════════════════════════════════════════════════════
    logic [7:0] msr_val;
    assign msr_val = 8'h30;  // CTS + DSR asserted

    // ══════════════════════════════════════════════════════════
    // Character timeout (NS16550A, FIFO mode only)
    // ══════════════════════════════════════════════════════════
    // The 16550A asserts an RX interrupt after the line has been
    // idle for >4 character-times, *even if* the RX FIFO is below
    // the configured trigger threshold.  Without this, paste-into-
    // console bursts shorter than the trigger sit silent forever.
    //
    // At 16x oversampling, one 10-bit character = 160 baud16x ticks;
    // 4 char-times = 640 ticks.
    localparam int CHAR_TIMEOUT_TICKS = 640;
    localparam int TIMEOUT_W          = $clog2(CHAR_TIMEOUT_TICKS + 1);

    /* verilator lint_off UNUSEDSIGNAL */  /* read by the human-written counter body */
    logic [TIMEOUT_W-1:0] char_timeout_cnt;
    /* verilator lint_on UNUSEDSIGNAL */
    logic                 char_timeout;

    always_ff @(posedge i_clk) begin
        if (i_rst || rx_fifo_flush || !fcr_fifo_enable || rx_byte_complete || rbr_read) begin
            char_timeout_cnt <= '0;
            char_timeout     <= 1'b0;
        end else if (baud16x_tick && !rx_fifo_empty) begin
            // Only count while the FIFO has data waiting — otherwise
            // the counter would re-arm on every drain and fire spurious
            // timeouts at idle (~2880 Hz of ISR wake-ups at 115200).
            // Increment in baud-clock domain so the timeout window is
            // 4 character-times regardless of system clock frequency.
            if (char_timeout_cnt == TIMEOUT_W'(CHAR_TIMEOUT_TICKS - 1)) begin
                char_timeout <= 1'b1;            // latch and freeze cnt
            end else if (!char_timeout) begin
                char_timeout_cnt <= char_timeout_cnt + 1'b1;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // IIR — Interrupt Identification Register
    // ══════════════════════════════════════════════════════════
    // bits 7:6  FIFO status  (00 = 16450, 11 = 16550A FIFO mode)
    // bits 3:0  interrupt id (priority encoded):
    //             0100  RX data above trigger
    //             1100  Character timeout (FIFO mode only)
    //             0010  THRE
    //             0001  None pending
    logic [7:0] iir;
    logic [1:0] iir_fifo_bits;
    assign iir_fifo_bits = fcr_fifo_enable ? 2'b11 : 2'b00;

    // RX-data interrupt: in FIFO mode, fires once level ≥ trigger.
    // In bypass mode, fires whenever rx_ready is set (single-byte).
    logic rx_data_int;
    assign rx_data_int = fcr_fifo_enable
                         ? ({1'b0, rx_fifo_level} >= {1'b0, trigger_threshold})
                         : rx_ready;

    logic irq_rx, irq_timeout, irq_tx;
    assign irq_rx      = ier[0] & rx_data_int;
    assign irq_timeout = ier[0] & char_timeout;   // ERBFI gates both
    assign irq_tx      = ier[1] & thre_int;

    always_comb begin
        if (irq_rx)
            iir = {iir_fifo_bits, 2'b00, 4'b0100};
        else if (irq_timeout)
            iir = {iir_fifo_bits, 2'b00, 4'b1100};
        else if (irq_tx)
            iir = {iir_fifo_bits, 2'b00, 4'b0010};
        else
            iir = {iir_fifo_bits, 2'b00, 4'b0001};
    end

    assign o_irq = (irq_rx | irq_timeout | irq_tx) & mcr[3];

    // ══════════════════════════════════════════════════════════
    // THRE interrupt tracking — rising-edge detect on LSR.THRE
    // ══════════════════════════════════════════════════════════
    // Bypass: thre_state = !tx_active (matches original behavior).
    // FIFO:   thre_state = tx_fifo_empty (16550A semantics —
    //         interrupt fires when the FIFO drains and the host
    //         can refill it).  thre_bit already encodes both.
    logic thre_state_q;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            thre_state_q <= 1'b1;     // resets agree with THRE-high
        else
            thre_state_q <= thre_bit;
    end

    logic thre_rise;
    assign thre_rise = thre_bit && !thre_state_q;

    logic iir_read_thre;
    assign iir_read_thre = read_first && (reg_sel == 3'd2)
                           && !irq_rx && !irq_timeout && irq_tx;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            thre_int <= 1'b0;
        else if (thre_rise)
            thre_int <= 1'b1;
        else if (thr_write || iir_read_thre)
            thre_int <= 1'b0;
    end

    // ══════════════════════════════════════════════════════════
    // Read data mux
    // ══════════════════════════════════════════════════════════
    logic [7:0] rdata_byte;

    // RBR source: in FIFO mode, returns the FIFO head (combinational
    // out of spi_fifo); in bypass mode, the single-byte rbr register.
    logic [7:0] rbr_source;
    assign rbr_source = fcr_fifo_enable ? rx_fifo_rdata : rbr;

    always_comb begin
        case (reg_sel)
            3'd0: rdata_byte = dlab ? dll : rbr_source;
            3'd1: rdata_byte = dlab ? dlm : ier;
            3'd2: rdata_byte = iir;
            3'd3: rdata_byte = lcr;
            3'd4: rdata_byte = mcr;
            3'd5: rdata_byte = lsr;
            3'd6: rdata_byte = msr_val;
            3'd7: rdata_byte = scr;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_re)
            o_rdata <= {24'b0, rdata_byte};
    end

    // ══════════════════════════════════════════════════════════
    // Write logic
    // ══════════════════════════════════════════════════════════
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ier <= 8'd0;
            lcr <= 8'd0;
            mcr <= 8'd0;
            scr <= 8'd0;
            fcr <= 8'd0;
            dll <= DEFAULT_DIVISOR[7:0];
            dlm <= DEFAULT_DIVISOR[15:8];
        end else if (i_we) begin
            case (reg_sel)
                3'd0: if (dlab) dll <= i_wdata[7:0];
                      // THR write handled in TX logic
                3'd1: if (dlab) dlm <= i_wdata[7:0];
                      else      ier <= i_wdata[7:0];
                3'd2: begin
                    // FCR: only the latched bits are stored.  The
                    // RX/TX reset bits self-clear (driven straight
                    // through to rx_fifo_flush / tx_fifo_flush above).
                    fcr[0]   <= i_wdata[0];
                    fcr[2:1] <= 2'b00;
                    fcr[5:3] <= 3'b000;
                    fcr[7:6] <= i_wdata[7:6];
                end
                3'd3: lcr <= i_wdata[7:0];
                3'd4: mcr <= i_wdata[7:0];
                // 3'd5: LSR — read only
                // 3'd6: MSR — read only
                3'd7: scr <= i_wdata[7:0];
                default: ;
            endcase
        end
    end

endmodule
