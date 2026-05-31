// Penumbra SPI Master v2 — FIFO-capable SPI controller
//
// Memory-mapped I/O device (4 KB page, base assigned by autoconfig).
// 16550-style FIFO enable: DATA register works in single-byte polled
// mode (FIFO_EN=0) or FIFO burst mode (FIFO_EN=1).
//
// Register map (word-strided, addr[4:2] decode):
//   0x00  CAP          (R)     Version [7:0], FIFO depth [23:8]
//   0x04  STATUS       (R)     SPI busy/done, FIFO levels/flags
//   0x08  CONTROL      (R/W)   CS, mode, speed, FIFO_EN, flush
//   0x0C  DATA         (R/W)   TX/RX byte (single or FIFO)
//   0x10  XFER_COUNT   (R/W)   Transfer count + START
//   0x14  IRQ_STATUS   (R/W1C) XFER_DONE(latched), thresholds(live)
//   0x18  IRQ_ENABLE   (R/W)   Per-source IRQ mask
//
// See doc/system/devices/spi.md (software interface) and
// doc/hardware/spi-hardware.md (internal state machines).
//
// Bus protocol: 1-cycle read latency, 0-cycle write.

// verilator lint_off UNUSEDSIGNAL

module spi
    import penumbra_pkg::*;
#(
    parameter int CLK_FREQ     = 25_000_000, // system clock feeding this block
    // Target SCLK frequencies — the dividers below are derived so SCLK
    // lands at or below these for ANY CLK_FREQ, so the SD interface stops
    // being silently pinned to whatever clock the FPGA closed timing at.
    //
    // FAST = 6.25 MHz is a CONSERVATIVE data clock, 1/4 of the 25 MHz
    // SD-SPI spec ceiling — NOT a measured board limit.  It was kept at
    // 6.25 MHz across the 12.5->25 MHz system-clock bump for stability
    // margin; 12.5 MHz is believed in-spec but has never been tested on
    // the ULX3S SD lines.  Raising this toward 12.5/25 MHz is a likely
    // 2-4x SD throughput win, gated on a real SD read/write check.
    // SLOW = the SD init clock (spec: < 400 kHz).
    parameter int SCLK_FAST_HZ = 6_250_000,
    parameter int SCLK_SLOW_HZ = 400_000,
    parameter     FIFO_DEPTH   = 512,         // TX and RX FIFO depth (pow2)

    // SCLK = CLK_FREQ / (2*(DIV+1)).  Invert it for DIV and round the
    // DIVISOR *up* (ceil): that yields the smallest DIV with SCLK <=
    // target.  Rounding the other way would push SCLK *over* target and
    // overclock the card past spec.  ceil(a/b) = (a + b - 1) / b.
    // Overridable so the sim testbench can force a fast slow-path.
    parameter logic [15:0] FAST_DIV =
        16'(((CLK_FREQ + 2*SCLK_FAST_HZ - 1) / (2*SCLK_FAST_HZ)) - 1),
    parameter logic [15:0] SLOW_DIV =
        16'(((CLK_FREQ + 2*SCLK_SLOW_HZ - 1) / (2*SCLK_SLOW_HZ)) - 1)
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface ────────────────────────────────
    input  logic [31:0] i_addr,     // Full byte address (uses [4:2])
    input  logic [31:0] i_wdata,    // Write data
    input  logic        i_we,       // Write enable
    input  logic        i_re,       // Read enable
    output logic [31:0] o_rdata,    // Read data (1-cycle latency)
    output logic        o_busy,     // Access in progress

    // ── Physical SPI pins ──────────────────────────────────
    output logic        o_sclk,     // SPI clock
    output logic        o_mosi,     // Master Out, Slave In
    input  logic        i_miso,     // Master In, Slave Out
    output logic        o_cs0,      // Directly from CONTROL register
    output logic        o_cs1,

    // ── Interrupt output ───────────────────────────────────
    output logic        o_irq
);

    // ── Register select from word-aligned address ───────────
    logic [2:0] reg_sel;
    assign reg_sel = i_addr[4:2];

    localparam REG_CAP         = 3'd0;  // 0x00
    localparam REG_STATUS      = 3'd1;  // 0x04
    localparam REG_CONTROL     = 3'd2;  // 0x08
    localparam REG_DATA        = 3'd3;  // 0x0C
    localparam REG_XFER_COUNT  = 3'd4;  // 0x10
    localparam REG_IRQ_STATUS  = 3'd5;  // 0x14
    localparam REG_IRQ_ENABLE  = 3'd6;  // 0x18

    // ── FIFO level width ───────────────────────────────────
    localparam FIFO_LEVEL_BITS = $clog2(FIFO_DEPTH) + 1;

    // ══════════════════════════════════════════════════════════
    // Control register
    // ══════════════════════════════════════════════════════════
    logic [15:0] control;
    // Field extraction
    logic cs0, cs1, cpol, cpha, fast, fifo_en;
    assign cs0     = control[0];
    assign cs1     = control[1];
    assign cpol    = control[4];
    assign cpha    = control[5];
    assign fast    = control[6];
    assign fifo_en = control[7];

    // ── Clock divider selection ─────────────────────────────
    logic [15:0] clkdiv;
    assign clkdiv = fast ? FAST_DIV : SLOW_DIV;

    // ── IRQ enable register ─────────────────────────────────
    logic [2:0] irq_enable;

    // ══════════════════════════════════════════════════════════
    // TX and RX FIFOs
    // ══════════════════════════════════════════════════════════
    logic       tx_wr, tx_rd, tx_full, tx_empty;
    logic [7:0] tx_wdata, tx_rdata;
    logic [FIFO_LEVEL_BITS-1:0] tx_level;
    logic       tx_flush;

    logic       rx_wr, rx_rd, rx_full, rx_empty;
    logic [7:0] rx_wdata, rx_rdata;
    logic [FIFO_LEVEL_BITS-1:0] rx_level;
    logic       rx_flush;

    spi_fifo #(.DEPTH(FIFO_DEPTH)) u_tx_fifo (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_wr    (tx_wr),
        .i_wdata (tx_wdata),
        .i_rd    (tx_rd),
        .o_rdata (tx_rdata),
        .o_full  (tx_full),
        .o_empty (tx_empty),
        .o_level (tx_level),
        .i_flush (tx_flush)
    );

    spi_fifo #(.DEPTH(FIFO_DEPTH)) u_rx_fifo (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_wr    (rx_wr),
        .i_wdata (rx_wdata),
        .i_rd    (rx_rd),
        .o_rdata (rx_rdata),
        .o_full  (rx_full),
        .o_empty (rx_empty),
        .o_level (rx_level),
        .i_flush (rx_flush)
    );

    // ══════════════════════════════════════════════════════════
    // SPI shift register — shared by single-byte and engine
    // ══════════════════════════════════════════════════════════
    logic        spi_busy;      // Shift register active
    logic        spi_done;      // Single-byte transfer complete
    logic [7:0]  shift_out;     // TX shift register (MSB first)
    logic [7:0]  shift_in;      // RX shift register (MSB first)
    logic [15:0] clk_count;     // Clock divider counter
    logic [3:0]  bit_count;     // 0..7 for 8 bits
    logic        sclk_reg;      // Current SCK output
    logic        phase;         // 0=leading edge half, 1=trailing edge half

    // Shift register completion pulse (active for 1 cycle)
    logic        shift_done;

    // ══════════════════════════════════════════════════════════
    // Transfer engine state machine
    // ══════════════════════════════════════════════════════════
    typedef enum logic [1:0] {
        ENG_IDLE       = 2'd0,
        ENG_LOAD_BYTE  = 2'd1,   // Wait for TX data, load shift register
        ENG_SHIFTING   = 2'd2,   // 8-bit shift in progress
        ENG_STORE_BYTE = 2'd3    // Wait for RX space, push received byte
    } engine_state_t;

    engine_state_t eng_state;
    logic [15:0]   eng_count;       // Remaining bytes
    logic          eng_active;      // Engine owns the shift register
    assign eng_active = (eng_state != ENG_IDLE);

    // XFER_DONE — latched, write-1-to-clear
    logic xfer_done;

    // ── Register write decode ───────────────────────────────
    logic wr_control, wr_data, wr_xfer_count, wr_irq_status, wr_irq_enable;
    assign wr_control    = i_we && (reg_sel == REG_CONTROL);
    assign wr_data       = i_we && (reg_sel == REG_DATA);
    assign wr_xfer_count = i_we && (reg_sel == REG_XFER_COUNT);
    assign wr_irq_status = i_we && (reg_sel == REG_IRQ_STATUS);
    assign wr_irq_enable = i_we && (reg_sel == REG_IRQ_ENABLE);

    // ── Read-side DATA pop (first cycle only) ───────────────
    logic read_first;
    logic access_pending;
    assign read_first = i_re && !access_pending;

    logic rd_data;
    assign rd_data = read_first && (reg_sel == REG_DATA);

    // ── Single-byte legacy rx_data register ─────────────────
    logic [7:0] rx_data;   // Holds last received byte in non-FIFO mode

    // ══════════════════════════════════════════════════════════
    // FIFO control signals
    // ══════════════════════════════════════════════════════════

    // TX FIFO write: CPU writes DATA when FIFO_EN=1
    assign tx_wr    = wr_data && fifo_en;
    assign tx_wdata = i_wdata[7:0];

    // TX FIFO read: engine pops in ENG_LOAD_BYTE
    // (directly controlled in the engine FSM below)

    // RX FIFO write: engine pushes after each byte shift
    // (directly controlled in the engine FSM below)

    // RX FIFO read: CPU reads DATA when FIFO_EN=1
    assign rx_rd = rd_data && fifo_en;

    // Flush: self-clearing bits in CONTROL
    assign tx_flush = wr_control && i_wdata[14] && fifo_en;
    assign rx_flush = wr_control && i_wdata[15] && fifo_en;

    // ══════════════════════════════════════════════════════════
    // Main sequential logic
    // ══════════════════════════════════════════════════════════
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            control    <= 16'h0003;     // Both CS deasserted, mode 0, slow, no FIFO
            irq_enable <= 3'b0;
            spi_busy   <= 1'b0;
            spi_done   <= 1'b0;
            shift_out  <= 8'h00;
            shift_in   <= 8'h00;
            clk_count  <= 16'd0;
            bit_count  <= 4'd0;
            sclk_reg   <= 1'b0;
            phase      <= 1'b0;
            shift_done <= 1'b0;
            eng_state  <= ENG_IDLE;
            eng_count  <= 16'd0;
            xfer_done  <= 1'b0;
            rx_data    <= 8'hFF;
        end else begin
            shift_done <= 1'b0;     // Default: pulse cleared each cycle

            // ── Register writes ────────────────────────────
            if (wr_control) begin
                // Bits [13:0] are persistent; [15:14] are flush (self-clearing)
                control <= i_wdata[15:0] & 16'h3FFF;
            end

            if (wr_irq_enable)
                irq_enable <= i_wdata[2:0];

            // W1C for XFER_DONE (bit 0 only)
            if (wr_irq_status && i_wdata[0])
                xfer_done <= 1'b0;

            // ── Single-byte transfer start (FIFO_EN=0) ────
            if (wr_data && !fifo_en && !eng_active) begin
                spi_busy  <= 1'b1;
                spi_done  <= 1'b0;
                shift_out <= i_wdata[7:0];
                shift_in  <= 8'h00;
                clk_count <= clkdiv;
                bit_count <= 4'd0;
                sclk_reg  <= cpol;
                phase     <= 1'b0;
            end

            // ── SPI shift register ─────────────────────────
            if (spi_busy) begin
                if (clk_count == 0) begin
                    phase     <= !phase;
                    clk_count <= clkdiv;
                    sclk_reg  <= !sclk_reg;
                    if (!phase) begin
                        // Leading edge
                        if (!cpha) shift_in  <= {shift_in[6:0], i_miso};
                        else       shift_out <= {shift_out[6:0], 1'b1};
                    end else begin
                        // Trailing edge
                        if (cpha)  shift_in  <= {shift_in[6:0], i_miso};
                        else       shift_out <= {shift_out[6:0], 1'b1};

                        bit_count <= bit_count + 1;

                        if (bit_count == 7) begin
                            spi_busy   <= 1'b0;
                            sclk_reg   <= cpol;
                            shift_done <= 1'b1;
                            // In non-FIFO mode, latch RX and set DONE
                            if (!eng_active) begin
                                spi_done <= 1'b1;
                                rx_data  <= shift_in;
                            end
                        end
                    end
                end else begin
                    clk_count <= clk_count - 1;
                end
            end

            // ── Transfer engine FSM ────────────────────────
            // Stalls on TX empty (LOAD_BYTE) and RX full (STORE_BYTE).
            // SPI clock pauses during stalls — no data loss or corruption.
            case (eng_state)
                ENG_IDLE: begin
                    if (wr_xfer_count && i_wdata[16] && fifo_en) begin
                        eng_count <= i_wdata[15:0];
                        if (i_wdata[15:0] != 0)
                            eng_state <= ENG_LOAD_BYTE;
                        else
                            // count=0 with START: immediate XFER_DONE
                            xfer_done <= 1'b1;
                    end
                end

                ENG_LOAD_BYTE: begin
                    // Wait for shift register free AND TX data available
                    if (!spi_busy && !tx_empty) begin
                        shift_out <= tx_rdata;
                        shift_in  <= 8'h00;
                        clk_count <= clkdiv;
                        bit_count <= 4'd0;
                        sclk_reg  <= cpol;
                        phase     <= 1'b0;
                        spi_busy  <= 1'b1;
                        eng_state <= ENG_SHIFTING;
                    end
                    // If tx_empty: stall here. TX_THRESH IRQ fires,
                    // handler refills TX FIFO, engine resumes.
                end

                ENG_SHIFTING: begin
                    if (shift_done)
                        // Byte shifted — move to store phase
                        eng_state <= ENG_STORE_BYTE;
                end

                ENG_STORE_BYTE: begin
                    // Wait for RX FIFO space
                    if (!rx_full) begin
                        // rx_wr fires combinationally this cycle
                        eng_count <= eng_count - 1;
                        if (eng_count == 1) begin
                            eng_state <= ENG_IDLE;
                            xfer_done <= 1'b1;
                        end else begin
                            eng_state <= ENG_LOAD_BYTE;
                        end
                    end
                    // If rx_full: stall here. RX_THRESH IRQ fires,
                    // handler drains RX FIFO, engine resumes.
                end

                default: eng_state <= ENG_IDLE;
            endcase
        end
    end

    // ── Engine TX FIFO pop — combinational, in ENG_LOAD_BYTE ──
    // Pop happens on the same cycle we load the shift register
    assign tx_rd = (eng_state == ENG_LOAD_BYTE) && !spi_busy && !tx_empty;

    // ── Engine RX FIFO push — in ENG_STORE_BYTE when space available ──
    assign rx_wr    = (eng_state == ENG_STORE_BYTE) && !rx_full;
    assign rx_wdata = shift_in;

    // ══════════════════════════════════════════════════════════
    // SPI pin outputs
    // ══════════════════════════════════════════════════════════
    assign o_sclk = sclk_reg;
    assign o_mosi = shift_out[7];       // MSB first
    assign o_cs0  = cs0;
    assign o_cs1  = cs1;

    // ══════════════════════════════════════════════════════════
    // IRQ logic — latched XFER_DONE + live watermarks
    // ══════════════════════════════════════════════════════════
    logic rx_thresh, tx_thresh;
    assign rx_thresh = fifo_en && (rx_level >= FIFO_LEVEL_BITS'(FIFO_DEPTH / 2));
    assign tx_thresh = fifo_en && (tx_level <= FIFO_LEVEL_BITS'(FIFO_DEPTH / 2));

    assign o_irq = (xfer_done & irq_enable[0])
                 | (rx_thresh & irq_enable[1])
                 | (tx_thresh & irq_enable[2]);

    // ══════════════════════════════════════════════════════════
    // Read data mux (registered for 1-cycle latency)
    // ══════════════════════════════════════════════════════════
    logic [31:0] rdata_next;

    // STATUS register assembly
    logic [31:0] status_val;
    assign status_val = {
        rx_full,                            // [31]    RX_FULL
        rx_empty,                           // [30]    RX_EMPTY
        tx_full,                            // [29]    TX_FULL
        tx_empty,                           // [28]    TX_EMPTY
        {(12 - FIFO_LEVEL_BITS){1'b0}}, rx_level, // [27:16] RX_LEVEL
        {(12 - FIFO_LEVEL_BITS){1'b0}}, tx_level, // [15:4]  TX_LEVEL
        2'b0,                               // [3:2]   reserved
        spi_done,                           // [1]     SPI_DONE
        spi_busy                            // [0]     SPI_BUSY
    };

    // CAP register
    logic [31:0] cap_val;
    assign cap_val = {8'b0, FIFO_DEPTH[15:0], 8'd1};  // version=1

    // IRQ_STATUS: bit 0 latched, bits 1-2 live
    logic [31:0] irq_status_val;
    assign irq_status_val = {29'b0, tx_thresh, rx_thresh, xfer_done};

    always_comb begin
        case (reg_sel)
            REG_CAP:        rdata_next = cap_val;
            REG_STATUS:     rdata_next = status_val;
            REG_CONTROL:    rdata_next = {16'b0, control};
            REG_DATA:       rdata_next = {24'b0, fifo_en ? rx_rdata : rx_data};
            REG_XFER_COUNT: rdata_next = {16'b0, eng_count};
            REG_IRQ_STATUS: rdata_next = irq_status_val;
            REG_IRQ_ENABLE: rdata_next = {29'b0, irq_enable};
            default:        rdata_next = 32'b0;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_re)
            o_rdata <= rdata_next;
        else
            o_rdata <= 32'b0;
    end

    // ── Read busy — 1-cycle latency (access_pending pattern) ─
    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_pending <= 1'b0;
        else if (i_re && !access_pending)
            access_pending <= 1'b1;
        else
            access_pending <= 1'b0;
    end

    assign o_busy = i_re && !access_pending;

endmodule
