// Penumbra Boot ROM — read-only memory for simulation
//
// Parameterizable size, word-addressed. Loads program.hex at init.
// A simple bus device: synchronous read, acknowledged one cycle after
// presentation. Read-only: writes are acknowledged and ignored.
//
// In the real SoC this would be replaced by actual ROM/flash
// containing the bootloader.

// verilator lint_off UNUSEDSIGNAL

module boot_rom
    import penumbra_pkg::*;
#(
    parameter ROM_WORDS = 16384  // Default 16K words = 64 KB
)
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,     // Byte address (lower bits used)
    input  logic        i_re,       // Read enable
    input  logic        i_we,       // Write enable (acknowledged, ignored)
    output logic [31:0] o_rdata,    // Read data, valid in the ack cycle
    output logic        o_ack
);

    localparam ADDR_BITS = $clog2(ROM_WORDS);

    logic [31:0] rom [0:ROM_WORDS-1];

`ifdef VERILATOR
    // Simulation: +rom_hex=<path> selects the image, so testbench programs
    // live in per-program files instead of contending for the boot ROM's
    // program.hex (the synthesis default below).
    string init_file;
`endif

    initial begin
        for (int i = 0; i < ROM_WORDS; i++)
            rom[i] = 32'b0;
`ifdef VERILATOR
        if (!$value$plusargs("rom_hex=%s", init_file))
            init_file = "program.hex";
        $readmemh(init_file, rom);
`else
        $readmemh("program.hex", rom);
`endif
    end

    // Word-addressed (lower bits index into ROM)
    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    // ── Synchronous read (1-cycle latency) ─────────────────
    logic [31:0] rdata_reg;
    always_ff @(posedge i_clk) begin
        rdata_reg <= rom[word_addr];
    end
    assign o_rdata = rdata_reg;

    // ── Bus handshake ──────────────────────────────────────
    // The ROM reads every cycle and ignores writes, so it needs only
    // the ack.
    bus_device_simple u_bus (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (i_re),
        .i_we    (i_we),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_read  (),
        .o_write (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_ack   (o_ack)
    );

endmodule

// verilator lint_on UNUSEDSIGNAL
