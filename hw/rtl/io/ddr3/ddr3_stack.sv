// DDR3 memory stack: the line-buffer SDRAM stack on a Wishbone burst memory
//
// The same adapter → CDC chain the line-buffer SDRAM stack builds
// (hw/rtl/io/sdram_line/), ending in a DDR3 controller's pipelined
// Wishbone port where that stack ends in sdram_ctrl. A 16-bit DDR3 part
// moves 16 bytes per burst, exactly the line the adapter holds, so a
// read is one burst and the response is the line as it arrives.
//
// The controller side of the crossing speaks the line contract
// (sdram_cdc): a request held valid until ready, then done, with the
// line on rsp_valid for a read. The Wishbone port speaks the same
// handshake under other names, so the glue between them is wiring and
// adds no cycle:
//   valid  → stb, held until the port stops stalling
//   ready  ← !stall
//   done   ← ack, for reads and writes alike, in request order
//   rsp_valid ← ack of a read: the request's we rides out on aux and
//               comes back with its ack
//   addr   → the burst holding the byte address
// A write is one word: it goes out replicated across the line, and its
// byte enables select that word's lanes, so the controller writes only
// the bytes the bus asked for.
//
// The bus cycle (o_wb_cyc) is open whenever the controller side is out
// of reset. A reset drops it, which cancels a request the controller
// still has in flight: its ack would otherwise arrive after the reset,
// where the crossing would take it for the next request's.
//
// The bus side runs on the system clock and the Wishbone side on the
// controller's; the crossing between them is sdram_cdc's.

module ddr3_stack #(
    parameter int ADDR_BITS = 25,   // the Wishbone port's burst address width
    parameter int AUX_WIDTH = 4     // the Wishbone port's aux width, at least 1
) (
    input  logic                  i_sys_clk,
    input  logic                  i_sys_rst,
    input  logic                  i_ctrl_clk,
    input  logic                  i_ctrl_rst,

    // ── System-bus slave port (system clock) ─────────────────
    input  logic [31:0]           i_addr,
    input  logic [31:0]           i_wdata,
    input  logic [3:0]            i_byte_en,
    input  logic                  i_we,
    input  logic                  i_re,
    output logic [31:0]           o_rdata,
    output logic                  o_ack,

    // ── Wishbone burst port (controller clock) ───────────────
    output logic                  o_wb_cyc,
    output logic                  o_wb_stb,
    output logic                  o_wb_we,
    output logic [ADDR_BITS-1:0]  o_wb_addr,
    output logic [127:0]          o_wb_data,
    output logic [15:0]           o_wb_sel,
    output logic [AUX_WIDTH-1:0]  o_wb_aux,
    input  logic                  i_wb_stall,
    input  logic                  i_wb_ack,
    input  logic [127:0]          i_wb_data,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [AUX_WIDTH-1:0]  i_wb_aux      // bit 0 is all the stack sends out
    /* verilator lint_on UNUSEDSIGNAL */
);
    localparam int LINE_BITS = 128;

    // ── Adapter ↔ CDC (system clock) ─────────────────────────
    logic                 sys_req_valid, sys_req_we, sys_req_ready;
    logic [31:0]          sys_req_addr,  sys_req_wdata;
    logic [3:0]           sys_req_byte_en;
    logic                 sys_rsp_valid, sys_rsp_ready, sys_done;
    logic [LINE_BITS-1:0] sys_rsp_data;

    sdram_bus_adapter #(
        .RSP_BITS      (LINE_BITS)
    ) u_adp (
        .i_clk         (i_sys_clk),
        .i_rst         (i_sys_rst),
        .i_addr        (i_addr),
        .i_wdata       (i_wdata),
        .i_byte_en     (i_byte_en),
        .i_we          (i_we),
        .i_re          (i_re),
        .o_rdata       (o_rdata),
        .o_ack         (o_ack),
        .o_req_valid   (sys_req_valid),
        .o_req_we      (sys_req_we),
        .o_req_addr    (sys_req_addr),
        .o_req_wdata   (sys_req_wdata),
        .o_req_byte_en (sys_req_byte_en),
        .i_req_ready   (sys_req_ready),
        .i_rsp_valid   (sys_rsp_valid),
        .i_rsp_data    (sys_rsp_data),
        .o_rsp_ready   (sys_rsp_ready),
        .i_done        (sys_done)
    );

    // ── CDC ↔ Wishbone port (controller clock) ───────────────
    logic                 ctrl_req_valid, ctrl_req_we, ctrl_req_ready;
    // The address above the port's reach is the bus decode's, and its low
    // two bits are the byte enables'.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [31:0]          ctrl_req_addr;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [31:0]          ctrl_req_wdata;
    logic [3:0]           ctrl_req_byte_en;
    logic                 ctrl_rsp_valid, ctrl_done;
    logic [LINE_BITS-1:0] ctrl_rsp_data;

    /* verilator lint_off PINCONNECTEMPTY */
    sdram_cdc #(
        .RSP_BITS          (LINE_BITS)
    ) u_cdc (
        .i_sys_clk         (i_sys_clk),
        .i_sys_rst         (i_sys_rst),
        .i_sys_req_valid   (sys_req_valid),
        .i_sys_req_we      (sys_req_we),
        .i_sys_req_addr    (sys_req_addr),
        .i_sys_req_wdata   (sys_req_wdata),
        .i_sys_req_byte_en (sys_req_byte_en),
        .o_sys_req_ready   (sys_req_ready),
        .o_sys_rsp_valid   (sys_rsp_valid),
        .o_sys_rsp_data    (sys_rsp_data),
        .i_sys_rsp_ready   (sys_rsp_ready),
        .o_sys_done        (sys_done),
        .i_sd_clk          (i_ctrl_clk),
        .i_sd_rst          (i_ctrl_rst),
        .o_sd_req_valid    (ctrl_req_valid),
        .o_sd_req_we       (ctrl_req_we),
        .o_sd_req_addr     (ctrl_req_addr),
        .o_sd_req_wdata    (ctrl_req_wdata),
        .o_sd_req_byte_en  (ctrl_req_byte_en),
        .i_sd_req_ready    (ctrl_req_ready),
        .i_sd_rsp_valid    (ctrl_rsp_valid),
        .i_sd_rsp_data     (ctrl_rsp_data),
        .o_sd_rsp_ready    (),
        .i_sd_done         (ctrl_done)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // ── Glue: the line contract as Wishbone ──────────────────
    // A write's word goes out replicated across the line, and its byte
    // enables move to that word's lanes. Word k of a line is bits
    // [32k+31:32k] and bytes 4k..4k+3, as the adapter indexes the line a
    // read returns; sel bit j masks byte j.
    assign o_wb_cyc       = !i_ctrl_rst;
    assign o_wb_stb       = ctrl_req_valid;
    assign o_wb_we        = ctrl_req_we;
    assign o_wb_addr      = ctrl_req_addr[4 +: ADDR_BITS];
    assign o_wb_data      = {4{ctrl_req_wdata}};
    assign o_wb_sel       = 16'(ctrl_req_byte_en) << {ctrl_req_addr[3:2], 2'b00};
    assign o_wb_aux       = AUX_WIDTH'(ctrl_req_we);
    assign ctrl_req_ready = !i_wb_stall;

    assign ctrl_done      = i_wb_ack;
    assign ctrl_rsp_valid = i_wb_ack && !i_wb_aux[0];
    assign ctrl_rsp_data  = i_wb_data;

endmodule
