// ULX3S UART test — sends 'U' (0x55) continuously
//
// 'U' = 01010101 → on the wire: start(0) 1010101 0 stop(1)
// This alternating pattern is the classic UART diagnostic:
// any baud rate or framing error is immediately visible.

module ulx3s_utest (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    output logic       ftdi_rxd,
    output logic       wifi_en
);

    assign wifi_en = 1'b0;

    // Reset generator
    logic [15:0] rst_cnt = '0;
    logic        rst;
    always_ff @(posedge clk_25mhz) begin
        if (!rst_cnt[15])
            rst_cnt <= rst_cnt + 1;
    end
    assign rst = !rst_cnt[15];

    // Heartbeat
    logic [23:0] hb_cnt;
    always_ff @(posedge clk_25mhz) begin
        if (rst) hb_cnt <= '0;
        else     hb_cnt <= hb_cnt + 1;
    end
    assign led[0]   = hb_cnt[23];
    assign led[7:1] = '0;

    // UART TX
    logic tx_busy;

    uart_tx #(
        .CLK_FREQ  (25_000_000),
        .BAUD_RATE (115_200)
    ) u_uart_tx (
        .i_clk   (clk_25mhz),
        .i_rst   (rst),
        .i_data  (8'h55),        // 'U' — always
        .i_valid (!tx_busy),     // send continuously
        .o_busy  (tx_busy),
        .o_tx    (ftdi_rxd)
    );

endmodule
