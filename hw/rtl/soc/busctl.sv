// Penumbra Bus Controller — sysreg device for autoconfig and bus reset
//
// Sysreg device 4 (SYSDEV_BUS). Controls the Penumbra Bus `rst` and
// `cfg` signals used by the autoconfig device discovery protocol.
//
// Register map:
//   0  BUSCTL  — Bit 0: RST (sticky — software asserts and deasserts)
//                Bit 1: CFG_EN (enable config chain + config address decode)
//
// Both bits are plain R/W. Software controls RST timing: write 1 to
// assert bus reset, delay as needed, write 0 to deassert. This avoids
// hardware timing logic and works correctly with slow async bus devices.
// CFG_EN directly drives o_cfg_en — when set, the config address range
// (0xFE00_0000) is active and the cfg daisy chain is enabled.
//
// Discrete 74xx: two flip-flops + one OR gate (hw reset | sw reset).

// verilator lint_off UNUSEDSIGNAL

module busctl
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Sysreg interface ───────────────────────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata,

    // ── Bus control outputs ────────────────────────────────
    output logic        o_bus_rst,     // Active-high, software-controlled duration
    output logic        o_cfg_en       // Config chain + address decode enable
);

    // ── BUSCTL register ────────────────────────────────────
    logic bus_rst;
    logic cfg_en;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            bus_rst <= 1'b0;
            cfg_en  <= 1'b0;
        end else if (i_sys_we && i_sys_reg == SYSREG_BUS_CTL) begin
            bus_rst <= i_sys_wdata[0];
            cfg_en  <= i_sys_wdata[1];
        end
    end

    // ── Outputs ────────────────────────────────────────────
    assign o_bus_rst = bus_rst;
    assign o_cfg_en  = cfg_en;

    // ── Read mux ───────────────────────────────────────────
    always_comb begin
        case (i_sys_reg)
            SYSREG_BUS_CTL: o_sys_rdata = {30'b0, cfg_en, bus_rst};
            default:         o_sys_rdata = 32'b0;
        endcase
    end

endmodule
