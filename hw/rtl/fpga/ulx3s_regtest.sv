// ULX3S registered handshake test — sends 'A' via tx_data/tx_valid regs
//
// Same handshake pattern as ulx3s_hello but with constant data.
// If this produces "AAAA..." the registered path works.
// If garbled, the issue is in the handshake timing.

module ulx3s_regtest (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    output logic       ftdi_rxd,
    output logic       wifi_en
);

    assign wifi_en = 1'b0;

    // Reset
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

    // UART
    logic [7:0] tx_data;
    logic       tx_valid;
    logic       tx_busy;

    uart_tx #(
        .CLK_FREQ  (25_000_000),
        .BAUD_RATE (115_200)
    ) u_uart_tx (
        .i_clk   (clk_25mhz),
        .i_rst   (rst),
        .i_data  (tx_data),
        .i_valid (tx_valid),
        .o_busy  (tx_busy),
        .o_tx    (ftdi_rxd)
    );

    // Registered handshake — same pattern as hello FSM
    always_ff @(posedge clk_25mhz) begin
        if (rst) begin
            tx_valid <= 1'b0;
            tx_data  <= 8'd0;
        end else if (tx_valid) begin
            tx_valid <= 1'b0;
        end else if (!tx_busy) begin
            tx_data  <= 8'h41;  // 'A'
            tx_valid <= 1'b1;
        end
    end

endmodule
