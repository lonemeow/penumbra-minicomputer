// Penumbra Simulation SPI Master — byte-at-a-time polled SPI controller
//
// Memory-mapped I/O device (4 KB page, base assigned by autoconfig).
// Implements the Penumbra SPI register protocol (CLASS_SPI):
//
//   0x00  DATA     (R/W)  TX/RX byte. Write starts transfer.
//   0x04  STATUS   (R)    Bit 0: BUSY, Bit 1: DONE
//   0x08  CONTROL  (R/W)  Bit 0: CS0, Bit 1: CS1, Bit 4: CPOL, Bit 5: CPHA
//   0x0C  CLKDIV   (R/W)  Clock divider (16-bit). SPI_CLK = CLK / (2*(CLKDIV+1))
//
// Simulation-only: transfers complete after SPI_BUSY_CYCLES (no real
// shift register or clock divider). The testbench can drive i_resp_data
// to supply MISO bytes (e.g., SD card emulation).
//
// Bus protocol: 1-cycle read latency, 0-cycle write (same as sim_uart).

// verilator lint_off UNUSEDSIGNAL

module sim_spi
    import penumbra_pkg::*;
#(
    parameter SPI_BUSY_CYCLES = 2   // cycles per byte transfer
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus interface ────────────────────────────────
    input  logic [31:0] i_addr,     // Full byte address (uses [3:2])
    input  logic [31:0] i_wdata,    // Write data (uses [15:0])
    input  logic        i_we,       // Write enable
    input  logic        i_re,       // Read enable
    output logic [31:0] o_rdata,    // Read data (1-cycle latency)
    output logic        o_busy,     // Access in progress (always 0)

    // ── SPI signals exposed to testbench ────────────────────
    output logic        o_cmd_valid,   // Pulses when DATA is written
    output logic [7:0]  o_cmd_data,    // Byte being sent (MOSI)
    input  logic        i_resp_valid,  // Testbench presents response byte
    input  logic [7:0]  i_resp_data,   // Byte from testbench (MISO)
    output logic        o_cs0,         // Directly from CONTROL register
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
    logic [7:0]  tx_data;       // Last written TX byte
    logic [7:0]  rx_data;       // Last received RX byte
    logic [7:0]  control;       // CS0, CS1, CPOL, CPHA
    logic [15:0] clkdiv;        // Clock divider

    // ── Transfer state machine ─────────────────────────────
    logic        spi_busy;
    logic        spi_done;
    logic [7:0]  busy_count;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tx_data    <= 8'h00;
            rx_data    <= 8'hFF;
            control    <= 8'h03;   // Both CS deasserted, mode 0
            clkdiv     <= 16'h00FF;
            spi_busy   <= 1'b0;
            spi_done   <= 1'b0;
            busy_count <= 0;
            o_cmd_valid <= 1'b0;
        end else begin
            o_cmd_valid <= 1'b0;

            if (spi_busy) begin
                if (busy_count == 0) begin
                    // Transfer complete
                    spi_busy <= 1'b0;
                    spi_done <= 1'b1;
                    // Latch response from testbench if available,
                    // otherwise default to 0xFF (SD card idle)
                    if (i_resp_valid)
                        rx_data <= i_resp_data;
                    else
                        rx_data <= 8'hFF;
                end else begin
                    busy_count <= busy_count - 8'd1;
                end
            end

            // Register writes
            if (i_we) begin
                case (reg_sel)
                    REG_DATA: begin
                        tx_data     <= i_wdata[7:0];
                        spi_busy    <= 1'b1;
                        spi_done    <= 1'b0;
                        busy_count  <= SPI_BUSY_CYCLES[7:0] - 8'd1;
                        o_cmd_valid <= 1'b1;
                        o_cmd_data  <= i_wdata[7:0];
                    end
                    REG_CONTROL: control <= i_wdata[7:0];
                    REG_CLKDIV:  clkdiv  <= i_wdata[15:0];
                    default: ;
                endcase
            end
        end
    end

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

    // ── Read busy — 1-cycle latency for registered read mux ──
    // Same pattern as sim_uart: busy on first cycle of read,
    // data valid on second cycle when busy clears.
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

    // ── CS outputs ─────────────────────────────────────────
    assign o_cs0 = control[0];
    assign o_cs1 = control[1];

endmodule
