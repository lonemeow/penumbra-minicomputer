// machine_penumbra2_sim — the gen2 machine with its sim devices attached.
//
// The Verilator wrapper of machine_penumbra2 (the sim-wrapper role in
// doc/internals/build-system.md): the board-independent machine plus the
// devices a program run needs:
//   - boot_rom — the ROM region at 0xFFFF_0000 holds the +rom_hex program;
//   - sdram_sim — the full v2 SDRAM stack (adapter + CDC + controller + sim
//     PHY + behavioral chip) backs the RAM region low, so the same
//     variable-latency memory model gen1's RTL sim uses now also exercises the
//     gen2 cache hierarchy. It runs on i_sdram_clk, driven faster than i_clk;
//   - a fixed-address sim UART (NS16450); and
//   - an autoconfig SD/SPI controller (sim_spi behind autoconfig_dev) on the
//     cfg daisy chain the machine's bus controller drives.
// All decode off the same external bus; their IRQs OR into the machine's
// interrupt line. The UART and SPI byte streams are brought to the top so the
// interactive testbench can bridge them to a host terminal and an SD-card
// emulator; the program runners (tb_penumbra2_prog, tb_penumbra2_intr) leave
// those idle and key only on the program-end pulse and commit port.

`ifndef PENUMBRA_CPU_VARIANT
`define PENUMBRA_CPU_VARIANT 0    // 0 = gen2 baseline, 1 = gen2.5
`endif

module machine_penumbra2_sim
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC  = 32'hFFFF_0000,
    parameter int          ROM_WORDS = 16384,            // 64 KiB boot ROM
    // 32 MiB SDRAM region (matches the ULX3S W9825 part the sim models). Big
    // enough that conformance programs' data and stacks never fault — the
    // widest gen2 test walks four pages up to 0x5000. The bus-fault tests pick
    // 0x0200_0000 as their "no device" address precisely because it sits just
    // past this region.
    parameter int          RAM_WORDS = 8 * 1024 * 1024
)(
    input  logic                  i_clk,
    // Separate SDRAM clock, driven faster than i_clk by the testbench to match
    // hardware's 25 MHz CPU / 100 MHz SDRAM ratio (hw/CLAUDE.md dual-clock sim),
    // so memory latency in CPU cycles tracks the FPGA. Tie to i_clk for a
    // same-rate setup.
    input  logic                  i_sdram_clk,
    input  logic                  i_rst,
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    // UART byte stream (sim_uart's TX/RX), bridged to a host terminal. The
    // program runner leaves the RX inputs unset (idle) and ignores the TX.
    output logic                  o_uart_tx_valid,
    output logic [7:0]            o_uart_tx_data,
    input  logic                  i_uart_rx_valid,
    input  logic [7:0]            i_uart_rx_data,
    output logic                  o_uart_rx_ack,

    // SPI master byte stream (sim_spi behind the autoconfig SD device), bridged
    // to the testbench's SD-card emulator. The program runner leaves the
    // response idle (i_spi_resp_valid=0 → MISO high) and ignores the rest.
    output logic                  o_spi_cmd_valid,
    output logic [7:0]            o_spi_cmd_data,
    input  logic                  i_spi_resp_valid,
    input  logic [7:0]            i_spi_resp_data,
    output logic                  o_spi_cs0,

    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class,
    output logic [31:0]           o_retire_pc,       // retiring instruction's PC (trace)
    output logic [31:0]           o_retire_sr,       // its architectural SR (trace)
    output logic                  o_fault_commit,    // a fault is taken this cycle (trace marker)
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic                  o_eret_commit,     // an ERET is committing (trace marker)
    output logic                  o_dc_commit,       // a drain-commit (EI/DI/WRSYS/ERET) retires (trace)
    output logic [31:0]           o_dc_commit_pc,    // its PC
    output logic [OPC_W-1:0]      o_dc_commit_op_class, // its op_class
    output logic                  o_branch_taken,    // EX branch resolve (trace)
    output logic [31:0]           o_branch_target,   // its target
    output logic [31:0]           o_branch_pc,       // the branch's own PC
    output logic [31:0]           o_ex_pc,           // pipeline occupancy: EX slot (trace)
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,          // MEM slot
    output logic                  o_mem_valid,
    output logic                  o_prog_end
);

    logic [31:0] bus_addr, bus_wdata, bus_rdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re, bus_we, bus_busy, bus_fault;

    // Per-slave read data / busy and address selects. The RAM region is backed
    // by the full v2 SDRAM stack (sdram_sim) and the ROM region by boot_rom, so
    // — unlike the old single compressed memory — each owns its own decode.
    logic [31:0] ram_rdata, rom_rdata, uart_rdata;
    logic        ram_busy, rom_busy, uart_busy, uart_irq;
    logic        ram_sel, rom_sel, uart_sel;
    logic        ram_sel_r, rom_sel_r, uart_sel_r;
    logic        irq_combined;

    // Bus-controller outputs (autoconfig cfg-enable / reset) from the machine.
    logic        bus_rst, bus_cfg_en;

    // Autoconfig SD/SPI device: autoconfig_dev wrapper + sim_spi master.
    logic [31:0] acfg_rdata, spi_dev_addr, spi_dev_wdata, spi_dev_rdata;
    // byte_en is delivered by autoconfig_dev but sim_spi is byte-oriented and
    // does not consume it.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        ac_spi_busy, ac_spi_sel, acfg_sel_r, spi_irq;
    logic        spi_dev_we, spi_dev_re, spi_dev_busy;

    machine_penumbra2 #(
        .RESET_PC(RESET_PC),
        .CPU_VARIANT(`PENUMBRA_CPU_VARIANT),
        // Sim machine identity: name "Simulator", 25 MHz — matches the
        // runners' clock and what isa/test_machid checks.
        .MACH_NAME0(32'h756D6953),   // "Simu"
        .MACH_NAME1(32'h6F74616C),   // "lato"
        .MACH_NAME2(32'h00000072),   // "r\0\0\0"
        .CPU_FREQ  (32'd25_000_000)
    ) u_machine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(irq_combined), .i_timer_irq(i_timer_irq),
        .o_bus_addr(bus_addr), .o_bus_wdata(bus_wdata),
        .o_bus_byte_en(bus_byte_en),
        .o_bus_re(bus_re), .o_bus_we(bus_we),
        .i_bus_rdata(bus_rdata), .i_bus_busy(bus_busy),
        .i_bus_fault(bus_fault),
        .o_bus_rst(bus_rst), .o_bus_cfg_en(bus_cfg_en),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid),
        .o_retire_op_class(o_retire_op_class),
        .o_retire_pc(o_retire_pc), .o_retire_sr(o_retire_sr),
        .o_fault_commit(o_fault_commit), .o_fault_vec(o_fault_vec),
        .o_eret_commit(o_eret_commit),
        .o_dc_commit(o_dc_commit), .o_dc_commit_pc(o_dc_commit_pc),
        .o_dc_commit_op_class(o_dc_commit_op_class),
        .o_branch_taken(o_branch_taken), .o_branch_target(o_branch_target),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_ex_valid(o_ex_valid),
        .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid),
        .o_prog_end(o_prog_end)
    );

    // ── Bus fabric ───────────────────────────────────────────────
    // Each slave owns its address decode: the SDRAM-backed RAM region at
    // RAM_BASE, the boot ROM at ROM_BASE, the UART's 4 KB page at UART_BASE,
    // and the autoconfig device (config space while cfg-enabled, then its
    // assigned base). No fabric-side map — what is mapped is discovered at
    // boot, so a no-device access is the absence of any claim (bus-protocol.md).
    localparam int UART_PAGE_SIZE = 4096;

    bus_devsel #(.BASE(RAM_BASE),  .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel  (.i_addr(bus_addr), .o_sel(ram_sel));
    bus_devsel #(.BASE(ROM_BASE),  .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel  (.i_addr(bus_addr), .o_sel(rom_sel));
    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(bus_addr), .o_sel(uart_sel));

    // Registered selects align the read-data mux with the slaves' registered
    // read latency; busy and fault stay combinational (current cycle). A held
    // SDRAM transaction keeps its select asserted through the busy-drop cycle,
    // so the one registered mux serves both the fixed-latency ROM/UART and the
    // variable-latency SDRAM.
    always_ff @(posedge i_clk) begin
        ram_sel_r  <= ram_sel;
        rom_sel_r  <= rom_sel;
        uart_sel_r <= uart_sel;
        acfg_sel_r <= ac_spi_sel;
    end

    assign irq_combined = i_irq | uart_irq | spi_irq;

    // TODO(human): OR-combine the slave responses onto the external bus.
    // Drive three signals from the per-slave outputs declared/wired above:
    //   bus_rdata — read-data mux. Each slave masks its rdata by its select so
    //               exactly one drives; use the *registered* selects
    //               (ram_sel_r / rom_sel_r / uart_sel_r / acfg_sel_r) so the
    //               mux lands on the cycle the slave's registered read is valid.
    //               Slaves: ram_rdata, rom_rdata, uart_rdata, acfg_rdata.
    //   bus_busy  — OR of each selected slave's busy, using the *combinational*
    //               selects (ram_sel / rom_sel / uart_sel / ac_spi_sel) so the
    //               core stalls the same cycle the slave needs time. Slaves:
    //               ram_busy, rom_busy, uart_busy, ac_spi_busy.
    //   bus_fault — an active access (bus_re | bus_we) that no slave claims.
    // See machine_sim.sv's "Bus response OR-combine" for the gen1 form this
    // mirrors (acfg_busy there is just ac_spi_busy, as it is here).

    always_comb begin
        if (ram_sel_r)       bus_rdata = ram_rdata;
        else if (rom_sel_r)  bus_rdata = rom_rdata;
        else if (uart_sel_r) bus_rdata = uart_rdata;
        else if (acfg_sel_r) bus_rdata = acfg_rdata;
        else                 bus_rdata = 32'b0;
    end
    assign bus_busy  = (ram_sel    ? ram_busy    : 1'b0)
                     | (rom_sel    ? rom_busy    : 1'b0)
                     | (uart_sel   ? uart_busy   : 1'b0)
                     | (ac_spi_sel ? ac_spi_busy : 1'b0);
    assign bus_fault = (bus_re | bus_we) & !(ram_sel | rom_sel | uart_sel | ac_spi_sel);

    // ── RAM: full v2 SDRAM stack (adapter + CDC + controller + sim PHY +
    // behavioral W9825 chip). Drop-in bus shape; needs the faster i_sdram_clk.
    sdram_sim u_ram (
        .i_clk(i_clk), .i_sdram_clk(i_sdram_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_we(bus_we & ram_sel), .i_re(bus_re & ram_sel),
        .o_rdata(ram_rdata), .o_busy(ram_busy)
    );

    // ── Boot ROM: holds the +rom_hex program at ROM_BASE (0xFFFF_0000).
    boot_rom #(.ROM_WORDS(ROM_WORDS)) u_rom (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_re(bus_re & rom_sel),
        .o_rdata(rom_rdata), .o_busy(rom_busy)
    );

    // Sim UART (NS16450, no FIFO). Its TX/RX byte stream is brought to the top.
    sim_uart u_uart (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata),
        .i_we(bus_we & uart_sel), .i_re(bus_re & uart_sel),
        .o_rdata(uart_rdata), .o_busy(uart_busy),
        .o_tx_valid(o_uart_tx_valid), .o_tx_data(o_uart_tx_data),
        .i_rx_valid(i_uart_rx_valid), .i_rx_data(i_uart_rx_data),
        .o_rx_ack(o_uart_rx_ack),
        .o_irq(uart_irq)
    );

    // ── Autoconfig SD/SPI controller ─────────────────────────────
    // sim_spi (the SPI master) behind autoconfig_dev (ACFG_CLASS_SD), the sole
    // device on the cfg daisy chain the machine's bus controller drives. The
    // ROM's autoconfig discovers it, assigns it a base, then drives it; its
    // byte stream bridges to the testbench's SD-card emulator. i_cfg_in =
    // bus_cfg_en makes it the first link; o_cfg_out dangles (chain end → the
    // next probe faults → "no more devices").
    /* verilator lint_off PINCONNECTEMPTY */
    autoconfig_dev #(
        .DEV_CLASS(ACFG_CLASS_SD), .DEV_SIZE(32'd4096), .DEV_ID(32'd0),
        .DEV_NAME0(32'h00004453)   // "SD\0\0" packed LE
    ) u_ac_spi (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bus_rst(bus_rst), .i_cfg_en(bus_cfg_en), .i_cfg_in(bus_cfg_en),
        .o_cfg_out(),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_we(bus_we), .i_re(bus_re),
        .o_rdata(acfg_rdata), .o_busy(ac_spi_busy), .o_sel(ac_spi_sel),
        .o_dev_addr(spi_dev_addr), .o_dev_wdata(spi_dev_wdata),
        .o_dev_byte_en(spi_dev_byte_en),
        .o_dev_we(spi_dev_we), .o_dev_re(spi_dev_re),
        .i_dev_rdata(spi_dev_rdata), .i_dev_busy(spi_dev_busy)
    );

    sim_spi u_spi (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(spi_dev_addr), .i_wdata(spi_dev_wdata),
        .i_we(spi_dev_we), .i_re(spi_dev_re),
        .o_rdata(spi_dev_rdata), .o_busy(spi_dev_busy),
        .o_cmd_valid(o_spi_cmd_valid), .o_cmd_data(o_spi_cmd_data),
        .i_resp_valid(i_spi_resp_valid), .i_resp_data(i_spi_resp_data),
        .o_cs0(o_spi_cs0), .o_cs1(), .o_irq(spi_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

endmodule
