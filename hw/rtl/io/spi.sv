// Penumbra SPI Master — byte-at-a-time polled SPI controller
//
// Memory-mapped I/O device (4 KB page, base assigned by autoconfig).
// Same register protocol as sim_spi (CLASS_SPI / CLASS_SD):
//
//   0x00  DATA     (R/W)  TX/RX byte. Write starts transfer.
//   0x04  STATUS   (R)    Bit 0: BUSY, Bit 1: DONE
//   0x08  CONTROL  (R/W)  Bit 0: CS0, Bit 1: CS1, Bit 4: CPOL, Bit 5: CPHA
//   0x0C  CLKDIV   (R/W)  Clock divider (16-bit). SPI_CLK = CLK / (2*(CLKDIV+1))
//
// Real hardware SPI: full-duplex shift register with configurable clock
// divider. One byte transferred per DATA write (8 SCK cycles).
//
// Bus protocol: 1-cycle read latency, 0-cycle write (same as sim_spi).

module spi
    import penumbra_pkg::*;
#(
    parameter DEFAULT_CLKDIV = 16'd63   // Reset divider (safe slow speed for SD init)
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface ────────────────────────────────
    input  logic [31:0] i_addr,     // Full byte address (uses [3:2])
    input  logic [31:0] i_wdata,    // Write data (uses [15:0])
    input  logic        i_we,       // Write enable
    input  logic        i_re,       // Read enable
    output logic [31:0] o_rdata,    // Read data (1-cycle latency)
    output logic        o_busy,     // Access in progress

    // ── Physical SPI pins ──────────────────────────────────
    output logic        o_sclk,     // SPI clock
    output logic        o_mosi,     // Master Out, Slave In
    input  logic        i_miso,     // Master In, Slave Out
    output logic        o_cs0,      // Directly from CONTROL register
    output logic        o_cs1
);

    // ── Register select from word-aligned address ───────────
    logic [1:0] reg_sel;
    assign reg_sel = i_addr[3:2];

    localparam REG_DATA    = 2'd0;
    localparam REG_STATUS  = 2'd1;
    localparam REG_CONTROL = 2'd2;
    localparam REG_CLKDIV  = 2'd3;

    // ── Registers ──────────────────────────────────────────
    logic [7:0]  tx_data;       // Last written TX byte (latched on write)
    logic [7:0]  rx_data;       // Last received RX byte (valid when done)
    logic [7:0]  control;       // CS0, CS1, CPOL, CPHA
    logic [15:0] clkdiv;        // Clock divider value

    // ── SPI transfer state ─────────────────────────────────
    logic        spi_busy;      // Transfer in progress
    logic        spi_done;      // Transfer complete (cleared on next DATA write)
    logic [7:0]  shift_out;     // TX shift register (MSB shifted out first)
    logic [7:0]  shift_in;      // RX shift register (MSB shifted in first)
    logic [15:0] clk_count;     // Clock divider counter (counts down to 0)
    logic [3:0]  bit_count;     // Counts 0..7 for 8 bits
    logic        sclk_reg;      // Current SCK output state
    logic        phase;         // 0 = first half (leading edge), 1 = second half (trailing edge)

    // ── CPOL/CPHA from control register ────────────────────
    logic cpol, cpha;
    assign cpol = control[4];
    assign cpha = control[5];

    // ── Register writes + SPI transfer engine ──────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_data   <= 8'h00;
            rx_data   <= 8'hFF;
            control   <= 8'h03;          // Both CS deasserted, mode 0
            clkdiv    <= DEFAULT_CLKDIV;
            spi_busy  <= 1'b0;
            spi_done  <= 1'b0;
            shift_out <= 8'h00;
            shift_in  <= 8'h00;
            clk_count <= 16'd0;
            bit_count <= 4'd0;
            sclk_reg  <= 1'b0;
            phase     <= 1'b0;
        end else begin
            // ── Register writes ────────────────────────────
            if (i_we) begin
                case (reg_sel)
                    REG_DATA: begin
                        tx_data   <= i_wdata[7:0];
                        // Start a new transfer
                        spi_busy  <= 1'b1;
                        spi_done  <= 1'b0;
                        shift_out <= i_wdata[7:0];
                        shift_in  <= 8'h00;
                        clk_count <= clkdiv;
                        bit_count <= 4'd0;
                        sclk_reg  <= cpol;  // Idle polarity
                        phase     <= 1'b0;
                    end
                    REG_CONTROL: control <= i_wdata[7:0];
                    REG_CLKDIV:  clkdiv  <= i_wdata[15:0];
                    default: ;
                endcase
            end

            if (spi_busy) begin
                clk_count <= clk_count - 1;
                if (clk_count == 0) begin
                    phase     <= !phase;
                    clk_count <= clkdiv;
                    sclk_reg  <= !sclk_reg;
                    if (!phase) begin
                        // Phase rising edge
                        if (!cpha) shift_in  <= {shift_in[6:0], i_miso};
                        else       shift_out <= {shift_out[6:0], 1'b1};
                    end else begin
                        // Phase falling edge
                        if (cpha)  shift_in  <= {shift_in[6:0], i_miso};
                        else       shift_out <= {shift_out[6:0], 1'b1};

                        bit_count <= bit_count + 1;

                        if (bit_count == 7) begin
                            spi_busy <= 1'b0;
                            spi_done <= 1'b1;
                            rx_data  <= shift_in;
                            sclk_reg <= cpol;
                        end
                    end
                end
            end
        end
    end

    // ── SPI pin outputs ────────────────────────────────────
    assign o_sclk = sclk_reg;           // Idle state set by cpol init
    assign o_mosi = shift_out[7];       // MSB first
    assign o_cs0  = control[0];
    assign o_cs1  = control[1];

    // ── Read mux (registered for 1-cycle latency) ──────────
    logic [31:0] rdata_next;
    always_comb begin
        case (reg_sel)
            REG_DATA:    rdata_next = {24'b0, rx_data};
            REG_STATUS:  rdata_next = {30'b0, spi_done, spi_busy};
            REG_CONTROL: rdata_next = {24'b0, control};
            REG_CLKDIV:  rdata_next = {16'b0, clkdiv};
            default:     rdata_next = 32'b0;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_re)
            o_rdata <= rdata_next;
        else
            o_rdata <= 32'b0;
    end

    // ── Read busy — 1-cycle latency (access_pending pattern) ─
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

endmodule
