// Penumbra Simulation UART — 16450-compatible for Verilator
//
// Memory-mapped I/O device at 0xFF00_0000 (4 KB page).
// Implements all 8 standard NS16450 registers at 4-byte (word)
// stride, data in bits [7:0] of each 32-bit word. Compatible
// with NetBSD com(4) driver using reg-shift=2, reg-io-width=4.
//
// Simulation-only: TX completes after TX_BUSY_CYCLES (default 2),
// not instant. This ensures polling code exercises the THRE check.
// RX uses a simple valid/ack handshake with the testbench.
// For real hardware, replace with a proper UART that adds
// baud rate generation from DLL/DLM and start/stop/parity bits.
//
// Bus protocol: 1-cycle read latency, 0-cycle write (same as
// simple_mem). Read side-effects (RBR clear, IIR clear) fire
// on the first cycle of the access only.
//
// Register map (word offsets from base):
//   0x000  RBR/THR (DLAB=0)  or  DLL (DLAB=1)
//   0x004  IER     (DLAB=0)  or  DLM (DLAB=1)
//   0x008  IIR (read)  /  FCR (write, ignored)
//   0x00C  LCR  (DLAB bit = bit 7)
//   0x010  MCR  (OUT2 = bit 3 = master IRQ enable)
//   0x014  LSR  (bit 0=DR, bit 5=THRE, bit 6=TEMT)
//   0x018  MSR  (CTS+DSR hardwired asserted)
//   0x01C  SCR  (scratch register, used for probe detection)

// verilator lint_off UNUSEDSIGNAL

module sim_uart
    import penumbra_pkg::*;
#(
    parameter TX_BUSY_CYCLES = 2170 // ~115200 baud at 25 MHz (217 cycles/bit × 10 bits/char)
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface ────────────────────────────────
    input  logic [31:0] i_addr,     // Full byte address (uses [4:2])
    input  logic [31:0] i_wdata,    // Write data (uses [7:0])
    input  logic        i_we,       // Write enable
    input  logic        i_re,       // Read enable
    output logic [31:0] o_rdata,    // Read data (1-cycle latency)
    output logic        o_busy,     // Access in progress (for STALL)

    // ── TX output (directly accessible by testbench) ────────
    output logic        o_tx_valid, // Pulses for 1 cycle on THR write
    output logic [7:0]  o_tx_data,  // Transmitted byte

    // ── RX input (directly driven by testbench) ─────────────
    input  logic        i_rx_valid, // Testbench presents a byte
    input  logic [7:0]  i_rx_data,  // Byte from testbench
    output logic        o_rx_ack,   // Pulses when UART accepts the byte

    // ── Interrupt output ────────────────────────────────────
    output logic        o_irq
);

    // ── Register select from word-aligned address ───────────
    logic [2:0] reg_sel;
    assign reg_sel = i_addr[4:2];

    // ══════════════════════════════════════════════════════════
    // Internal registers
    // ══════════════════════════════════════════════════════════
    logic [7:0] rbr;              // Receive buffer register
    logic [7:0] ier;              // Interrupt enable register
    logic [7:0] lcr;              // Line control register
    logic [7:0] mcr;              // Modem control register
    logic [7:0] scr;              // Scratch register
    logic [7:0] dll, dlm;         // Baud divisor (accepted, ignored in sim)
    logic       rx_ready;          // RX holding register has data
    logic       thre_int;          // TX holding register empty interrupt pending

    logic dlab;
    assign dlab = lcr[7];

    // ══════════════════════════════════════════════════════════
    // Bus protocol — 1-cycle read latency
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

    // Side-effect pulse: fires only on the first cycle of a read
    logic read_first;
    assign read_first = i_re && !access_pending;

    // ══════════════════════════════════════════════════════════
    // TX busy counter — simulates baud-rate delay
    // ══════════════════════════════════════════════════════════
    // After THR write, tx_busy counts down TX_BUSY_CYCLES before
    // THRE goes high again. Ensures polling code actually polls.
    logic [15:0] tx_busy;
    logic       tx_ready;
    assign tx_ready = (tx_busy == 0);

    // ══════════════════════════════════════════════════════════
    // LSR — Line Status Register (read-only, computed)
    // ══════════════════════════════════════════════════════════
    // Bit 0: DR   — data ready (RX has data)
    // Bit 5: THRE — TX holding register empty
    // Bit 6: TEMT — transmitter empty
    logic [7:0] lsr;
    assign lsr = {1'b0, tx_ready, tx_ready, 4'b0, rx_ready};

    // ══════════════════════════════════════════════════════════
    // MSR — Modem Status Register (read-only, hardwired)
    // ══════════════════════════════════════════════════════════
    // CTS (bit 4) + DSR (bit 5) asserted, no delta bits
    logic [7:0] msr_val;
    assign msr_val = 8'h30;

    // ══════════════════════════════════════════════════════════
    // IIR — Interrupt Identification Register (read-only)
    // ══════════════════════════════════════════════════════════
    // Priority: RX ready (highest) > TX empty > (no others)
    // Bit 0:   0 = interrupt pending, 1 = no interrupt
    // Bit 2:1: 10 = RX ready, 01 = TX empty
    // Bit 7:6: 00 = no FIFO (16450 mode)
    logic [7:0] iir;
    logic irq_rx, irq_tx;
    assign irq_rx = ier[0] & rx_ready;
    assign irq_tx = ier[1] & thre_int;

    always_comb begin
        if (irq_rx)
            iir = 8'h04;       // ID=10: RX data ready
        else if (irq_tx)
            iir = 8'h02;       // ID=01: TX holding register empty
        else
            iir = 8'h01;       // No interrupt pending
    end

    // ── IRQ output (active when any interrupt + OUT2 master enable)
    assign o_irq = (irq_rx | irq_tx) & mcr[3];

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

    // Registered read data (1-cycle latency)
    always_ff @(posedge i_clk) begin
        if (i_re)
            o_rdata <= {24'b0, rdata_byte};
    end

    // ══════════════════════════════════════════════════════════
    // TX logic — delayed by TX_BUSY_CYCLES
    // ══════════════════════════════════════════════════════════
    // THR write starts the busy counter. o_tx_valid pulses when
    // the counter reaches 1 (byte "transmitted"). THRE/TEMT in
    // LSR reflect tx_ready (counter == 0).
    logic thr_write;
    assign thr_write = i_we && !dlab && (reg_sel == 3'd0);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_busy    <= 16'b0;
            o_tx_valid <= 1'b0;
            o_tx_data  <= 8'b0;
        end else if (thr_write) begin
            tx_busy    <= TX_BUSY_CYCLES[15:0];
            o_tx_valid <= 1'b0;
            o_tx_data  <= i_wdata[7:0];
        end else if (tx_busy > 0) begin
            tx_busy    <= tx_busy - 16'd1;
            // Pulse tx_valid on the last busy cycle (transition to ready)
            o_tx_valid <= (tx_busy == 16'd1);
        end else begin
            o_tx_valid <= 1'b0;
        end
    end

    // ══════════════════════════════════════════════════════════
    // RX logic — testbench handshake
    // ══════════════════════════════════════════════════════════
    // Testbench holds i_rx_valid + i_rx_data until o_rx_ack.
    // UART accepts when holding register is empty.
    // Reading RBR (DLAB=0, reg 0) clears rx_ready.
    logic rbr_read;
    assign rbr_read = read_first && !dlab && (reg_sel == 3'd0);
    assign o_rx_ack = i_rx_valid && !rx_ready;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            rbr      <= 8'b0;
            rx_ready <= 1'b0;
        end else if (rbr_read) begin
            // CPU reads RBR — clear data ready
            rx_ready <= 1'b0;
        end else if (i_rx_valid && !rx_ready) begin
            // Accept new byte from testbench
            rbr      <= i_rx_data;
            rx_ready <= 1'b1;
        end
    end

    // ══════════════════════════════════════════════════════════
    // THRE interrupt tracking
    // ══════════════════════════════════════════════════════════
    // THRE fires on the edge when THR becomes empty (tx_busy
    // transitions from 1 to 0). Cleared when IIR is read with
    // THRE shown, or when THR is written again.
    logic tx_complete;
    assign tx_complete = (tx_busy == 16'd1);  // Will be 0 next cycle

    logic iir_read_thre;
    assign iir_read_thre = read_first && (reg_sel == 3'd2) && !irq_rx && irq_tx;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            thre_int <= 1'b0;
        else if (tx_complete)
            // TX just finished — set THRE interrupt
            thre_int <= 1'b1;
        else if (thr_write || iir_read_thre)
            // THR written (new TX started) or IIR read — clear it
            thre_int <= 1'b0;
    end

    // ══════════════════════════════════════════════════════════
    // Write logic — register updates
    // ══════════════════════════════════════════════════════════
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ier <= 8'b0;
            lcr <= 8'b0;
            mcr <= 8'b0;
            scr <= 8'b0;
            dll <= 8'b0;
            dlm <= 8'b0;
        end else if (i_we) begin
            case (reg_sel)
                3'd0: if (dlab) dll <= i_wdata[7:0];
                      // THR write handled in TX logic
                3'd1: if (dlab) dlm <= i_wdata[7:0];
                      else      ier <= i_wdata[7:0];
                // 3'd2: FCR — write accepted, ignored (no FIFO)
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

// verilator lint_on UNUSEDSIGNAL
