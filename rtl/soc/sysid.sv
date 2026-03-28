// Penumbra System ID — read-only machine identification register
//
// Sysreg device 1 (SYSDEV_SYS). Provides hardwired constants that
// software can read via RDSYS to identify the hardware platform and
// adapt at runtime (e.g., FPGA vs discrete 74xx build).
//
// Purely combinational — no clock, no state. On 74xx discrete this
// is literally pull-up/pull-down resistors on the sysreg data bus.

module sysid
    import penumbra_pkg::*;
(
    input  logic [3:0]  i_sys_reg,      // Register address within device
    output logic [31:0] o_sys_rdata     // Read data
);

    localparam logic [31:0] MACHINE_ID = 32'h0000_0001;  // Penumbra/1

    always_comb begin
        case (i_sys_reg)
            SYSREG_SYS_MACHID: o_sys_rdata = MACHINE_ID;
            default:            o_sys_rdata = 32'b0;
        endcase
    end

endmodule
