// Penumbra UART — NS16450-compatible with real serial I/O
//
// Drop-in replacement for sim_uart on real hardware. Same bus-side
// register interface (address decode, read/write protocol, interrupt
// logic), but with actual baud rate generation and serial TX/RX pins
// instead of testbench handshake signals.
//
// Register map (word offsets from base, word-strided):
//   0x000  RBR/THR (DLAB=0)  or  DLL (DLAB=1)
//   0x004  IER     (DLAB=0)  or  DLM (DLAB=1)
//   0x008  IIR (read)  /  FCR (write, ignored)
//   0x00C  LCR  (DLAB bit = bit 7)
//   0x010  MCR  (OUT2 = bit 3 = master IRQ enable)
//   0x014  LSR  (bit 0=DR, bit 5=THRE, bit 6=TEMT)
//   0x018  MSR  (CTS+DSR hardwired asserted)
//   0x01C  SCR  (scratch register)
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

module uart
    import penumbra_pkg::*;
#(
    parameter int CLK_FREQ = 25_000_000,
    parameter int REF_FREQ =  1_843_200    // advertised 16450 crystal
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface (same as sim_uart) ─────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
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
    logic       thre_int;

    logic dlab;
    assign dlab = lcr[7];

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

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ref_acc  <= '0;
            ref_tick <= 1'b0;
        end else begin
            int tmp = ref_acc + REF_FREQ;
            if (tmp >= CLK_FREQ) begin
                ref_acc  <= tmp - CLK_FREQ;
                ref_tick <= 1'b1;
            end else begin
                ref_acc <= tmp;
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
    logic        tx_ready;     // THR is empty (THRE/TEMT)

    assign tx_ready = !tx_active;

    // THR write detection
    logic thr_write;
    assign thr_write = i_we && !dlab && (reg_sel == 3'd0);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_shift      <= 10'h3FF; // all ones = idle
            tx_bit_idx    <= 4'd0;
            tx_sample_cnt <= 4'd0;
            tx_active     <= 1'b0;
            o_tx          <= 1'b1;    // idle high
        end else if (!tx_active && thr_write) begin
            // Load shift register: {stop=1, data[7:0], start=0}
            tx_shift      <= {1'b1, i_wdata[7:0], 1'b0};
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

    logic [3:0]  rx_sample_cnt;
    logic [2:0]  rx_bit_idx;
    logic [7:0]  rx_shift;

    typedef enum logic [2:0] {IDLE, START, DATA, STOP} state_e_rx;
    state_e_rx rx_state = IDLE;

    // RX state machine + RBR delivery
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            rbr           <= 8'd0;
            rx_ready      <= 1'b0;
            rx_state      <= IDLE;
        end else if (rbr_read) begin
            rx_ready <= 1'b0;
        end else if (baud16x_tick) begin
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
                    end
                end
            endcase
        end
    end

    // ══════════════════════════════════════════════════════════
    // LSR — Line Status Register
    // ══════════════════════════════════════════════════════════
    logic [7:0] lsr;
    assign lsr = {1'b0, tx_ready, tx_ready, 4'b0, rx_ready};

    // ══════════════════════════════════════════════════════════
    // MSR — Modem Status Register (hardwired)
    // ══════════════════════════════════════════════════════════
    logic [7:0] msr_val;
    assign msr_val = 8'h30;  // CTS + DSR asserted

    // ══════════════════════════════════════════════════════════
    // IIR — Interrupt Identification Register
    // ══════════════════════════════════════════════════════════
    logic [7:0] iir;
    logic irq_rx, irq_tx;
    assign irq_rx = ier[0] & rx_ready;
    assign irq_tx = ier[1] & thre_int;

    always_comb begin
        if (irq_rx)
            iir = 8'h04;       // RX data ready
        else if (irq_tx)
            iir = 8'h02;       // TX holding register empty
        else
            iir = 8'h01;       // No interrupt pending
    end

    assign o_irq = (irq_rx | irq_tx) & mcr[3];

    // ══════════════════════════════════════════════════════════
    // THRE interrupt tracking
    // ══════════════════════════════════════════════════════════
    logic tx_was_active;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            tx_was_active <= 1'b0;
        else
            tx_was_active <= tx_active;
    end

    logic tx_complete;
    assign tx_complete = tx_was_active && !tx_active;

    logic iir_read_thre;
    assign iir_read_thre = read_first && (reg_sel == 3'd2) && !irq_rx && irq_tx;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            thre_int <= 1'b0;
        else if (tx_complete)
            thre_int <= 1'b1;
        else if (thr_write || iir_read_thre)
            thre_int <= 1'b0;
    end

    // ══════════════════════════════════════════════════════════
    // Read data mux
    // ══════════════════════════════════════════════════════════
    logic [7:0] rdata_byte;

    always_comb begin
        case (reg_sel)
            3'd0: rdata_byte = dlab ? dll : rbr;
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
            dll <= DEFAULT_DIVISOR[7:0];
            dlm <= DEFAULT_DIVISOR[15:8];
        end else if (i_we) begin
            case (reg_sel)
                3'd0: if (dlab) dll <= i_wdata[7:0];
                      // THR write handled in TX logic
                3'd1: if (dlab) dlm <= i_wdata[7:0];
                      else      ier <= i_wdata[7:0];
                // 3'd2: FCR — accepted, ignored (no FIFO)
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
