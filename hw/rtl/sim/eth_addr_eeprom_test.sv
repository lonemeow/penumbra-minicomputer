// eth_addr_eeprom_test — module-test harness for the station address
// reader.
//
// The reader on an I2C master, with the bus's two lines brought out for a
// model of the EEPROM (tb_eth_addr_eeprom). The clock is slowed as
// i2c_master_test slows it, so that a quarter of the SCL period is ten
// cycles and the whole read a few thousand.

module eth_addr_eeprom_test (
    input  logic        i_clk,
    input  logic        i_rst,

    output logic [47:0] o_addr,
    output logic        o_valid,

    output logic        o_scl,
    input  logic        i_scl,
    output logic        o_sda,
    input  logic        i_sda
);

    logic       cmd_valid, cmd_ready, cmd_start, cmd_stop, cmd_read, cmd_nack;
    logic [7:0] cmd_wdata;
    logic       rsp_valid, rsp_nack, rsp_error;
    logic [7:0] rsp_rdata;

    eth_addr_eeprom #(
        .DEV_ADDR (7'h57)
    ) u_reader (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .o_cmd_valid (cmd_valid),
        .i_cmd_ready (cmd_ready),
        .o_cmd_start (cmd_start),
        .o_cmd_stop  (cmd_stop),
        .o_cmd_read  (cmd_read),
        .o_cmd_wdata (cmd_wdata),
        .o_cmd_nack  (cmd_nack),
        .i_rsp_valid (rsp_valid),
        .i_rsp_rdata (rsp_rdata),
        .i_rsp_nack  (rsp_nack),
        .i_rsp_error (rsp_error),
        .o_addr      (o_addr),
        .o_valid     (o_valid)
    );

    i2c_master #(
        .CLK_HZ (4_000_000),
        .SCL_HZ (100_000)
    ) u_i2c (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_cmd_valid (cmd_valid),
        .o_cmd_ready (cmd_ready),
        .i_cmd_start (cmd_start),
        .i_cmd_stop  (cmd_stop),
        .i_cmd_read  (cmd_read),
        .i_cmd_wdata (cmd_wdata),
        .i_cmd_nack  (cmd_nack),
        .o_rsp_valid (rsp_valid),
        .o_rsp_rdata (rsp_rdata),
        .o_rsp_nack  (rsp_nack),
        .o_rsp_error (rsp_error),
        .o_scl       (o_scl),
        .i_scl       (i_scl),
        .o_sda       (o_sda),
        .i_sda       (i_sda)
    );

endmodule
