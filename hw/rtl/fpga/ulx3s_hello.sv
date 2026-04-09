// ULX3S "Hello" — minimal FPGA board test
//
// Sends "Hello from Penumbra!\r\n" on the FTDI UART at 115200 8N1,
// repeating every ~1 second.  Blinks LED[0] as heartbeat.
// Holds ESP32 in reset so it doesn't contend on the UART pins.

module ulx3s_hello (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    output logic       ftdi_rxd,    // FPGA TX → FTDI RX → host
    output logic       wifi_en      // LOW = hold ESP32 in reset
);

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── Reset generator (hold reset for 2^16 clocks ≈ 2.6 ms) ─
    logic [15:0] rst_cnt = '0;
    logic        rst;

    always_ff @(posedge clk_25mhz) begin
        if (!rst_cnt[15])
            rst_cnt <= rst_cnt + 1;
    end
    assign rst = !rst_cnt[15];

    // ── Heartbeat (LED[0] toggles at ~1.5 Hz) ─────────────────
    logic [23:0] hb_cnt;
    always_ff @(posedge clk_25mhz) begin
        if (rst)
            hb_cnt <= '0;
        else
            hb_cnt <= hb_cnt + 1;
    end
    assign led[0] = hb_cnt[23];
    // Debug: LED[2]=pausing (should be ON for 1s between messages)
    //        LED[3]=msg_idx reached 22

    // ── UART TX instance ───────────────────────────────────────
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

    // ── Message ROM (combinational lookup) ──────────────────────
    localparam MSG_LEN = 22;

    logic [7:0] msg_byte;
    always_comb begin
        case (msg_idx)
            5'd0:  msg_byte = "H";
            5'd1:  msg_byte = "e";
            5'd2:  msg_byte = "l";
            5'd3:  msg_byte = "l";
            5'd4:  msg_byte = "o";
            5'd5:  msg_byte = " ";
            5'd6:  msg_byte = "f";
            5'd7:  msg_byte = "r";
            5'd8:  msg_byte = "o";
            5'd9:  msg_byte = "m";
            5'd10: msg_byte = " ";
            5'd11: msg_byte = "P";
            5'd12: msg_byte = "e";
            5'd13: msg_byte = "n";
            5'd14: msg_byte = "u";
            5'd15: msg_byte = "m";
            5'd16: msg_byte = "b";
            5'd17: msg_byte = "r";
            5'd18: msg_byte = "a";
            5'd19: msg_byte = "!";
            5'd20: msg_byte = 8'h0D;
            5'd21: msg_byte = 8'h0A;
            default: msg_byte = 8'h00;
        endcase
    end

    // ── Message sender ────────────────────────────────────────
    // Two-state machine: send all bytes, then wait ~1 second.

    logic [4:0]  msg_idx;
    logic [24:0] wait_cnt;
    logic        sending;  // 1 = sending message, 0 = waiting

    assign led[1] = tx_busy;
    assign led[2] = sending;
    assign led[3] = (msg_idx >= 5'd22);
    assign led[7:4] = '0;

    always_ff @(posedge clk_25mhz) begin
        if (rst) begin
            msg_idx  <= 5'd0;
            wait_cnt <= 25'd0;
            sending  <= 1'b1;
            tx_valid <= 1'b0;
            tx_data  <= 8'd0;
        end else if (sending) begin
            // Sending message bytes one at a time
            if (tx_valid) begin
                tx_valid <= 1'b0;
            end else if (!tx_busy) begin
                if (msg_idx >= 5'd22) begin
                    // All bytes sent — switch to waiting
                    sending  <= 1'b0;
                    wait_cnt <= 25'd0;
                end else begin
                    tx_data  <= msg_byte;
                    tx_valid <= 1'b1;
                    msg_idx  <= msg_idx + 1'b1;
                end
            end
        end else begin
            // Waiting ~1 second between messages
            tx_valid <= 1'b0;
            if (wait_cnt == 25'd24_999_999) begin
                sending <= 1'b1;
                msg_idx <= 5'd0;
            end else begin
                wait_cnt <= wait_cnt + 1'b1;
            end
        end
    end

endmodule
