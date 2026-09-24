// On-chip RAM: system RAM built from block RAM, for a board or a bring-up
// step with no external memory behind the bus.
//
// A simple bus device (bus_device_simple): an access does its work in its
// first cycle, a read capturing its word and a write committing the lanes
// its byte enables select, and is acknowledged in the next. The top
// decodes the device's window (bus_devsel); an address inside it indexes
// the array by word, and its upper bits are not looked at.

module onchip_ram #(
    parameter int WORDS = 32768      // 128 KiB
) (
    input  logic        i_clk,
    input  logic        i_rst,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,      // only the word index is used
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_re,
    input  logic        i_we,
    output logic [31:0] o_rdata,     // valid in the ack cycle
    output logic        o_ack
);

    localparam int ADDR_BITS = $clog2(WORDS);

    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    logic bus_read;
    logic bus_write;

    bus_device_simple u_bus (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (i_re),
        .i_we    (i_we),
        .o_read  (bus_read),
        .o_write (bus_write),
        .o_ack   (o_ack)
    );

    logic [31:0] mem [0:WORDS-1];
    logic [31:0] rdata_q;

    always_ff @(posedge i_clk) begin
        if (bus_write) begin
            for (int b = 0; b < 4; b++)
                if (i_byte_en[b])
                    mem[word_addr][8*b +: 8] <= i_wdata[8*b +: 8];
        end else if (bus_read) begin
            rdata_q <= mem[word_addr];
        end
    end

    assign o_rdata = rdata_q;

endmodule
