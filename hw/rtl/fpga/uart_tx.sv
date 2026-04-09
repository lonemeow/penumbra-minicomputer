// Penumbra UART transmitter — 8N1 serial output
//
// Shift-register UART TX with configurable baud rate.
// Idle-high (standard RS-232 levels on FTDI).
//
// Interface:
//   i_data[7:0] — byte to send (latched when i_valid && !o_busy)
//   i_valid     — pulse high for 1 cycle to begin transmission
//   o_busy      — high while transmitting (do not assert i_valid)
//   o_tx        — serial output (idle high)

module uart_tx #(
    parameter CLK_FREQ  = 25_000_000,
    parameter BAUD_RATE = 115_200
) (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [7:0] i_data,
    input  logic       i_valid,
    output logic       o_busy,
    output logic       o_tx
);

    localparam CLKS_PER_BIT = CLK_FREQ / BAUD_RATE;
    localparam CLK_LOAD_VAL = CLKS_PER_BIT - 1;

    typedef enum logic [2:0] {IDLE, START, DATA, STOP, CLEANUP} state_e;
    state_e state = IDLE;

    logic [15:0] clk_count = 0;
    logic [3:0]  bit_index = 0;
    logic [7:0]  tx_data   = 0;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state  <= IDLE;
            o_tx   <= 1'b1;
            o_busy <= 1'b0;
        end else begin
            case (state)
                IDLE: begin
                    if (i_valid && !o_busy) begin
                        tx_data   <= i_data;
                        o_busy    <= 1'b1;
                        o_tx      <= 1'b0;
                        bit_index <= 3'd0;
                        clk_count <= CLK_LOAD_VAL;
                        state     <= START;
                    end else begin
                        o_tx   <= 1'b1;
                        o_busy <= 1'b0;
                    end
                end
                START: begin
                    clk_count <= clk_count - 1;
                    if (clk_count == 0) begin
                        o_tx      <= tx_data[bit_index];
                        bit_index <= bit_index + 1;
                        clk_count <= CLK_LOAD_VAL;
                        state     <= DATA;
                    end
                end
                DATA: begin
                    clk_count <= clk_count - 1;
                    if (clk_count == 0) begin
                        if (bit_index < 8) begin
                            o_tx      <= tx_data[bit_index];
                            bit_index <= bit_index + 1;
                            clk_count <= CLK_LOAD_VAL;
                        end else begin
                            o_tx      <= 1'b1;
                            clk_count <= CLK_LOAD_VAL;
                            state     <= STOP;
                        end
                    end
                end
                STOP: begin
                    clk_count <= clk_count - 1;
                    if (clk_count == 0) begin
                        clk_count <= CLK_LOAD_VAL;
                        state     <= CLEANUP;
                    end
                end
                CLEANUP: begin
                    o_busy <= 1'b0;
                    state  <= IDLE;
                end
            endcase
        end
    end

endmodule
