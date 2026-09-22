// ECP5 primitive models with per-instance delays, for glitch-accurate
// event simulation of a synthesized netlist.
//
// Functionally identical to yosys' cells_sim_ecp5 models; every input
// pin additionally carries a transport delay (a per-instance routing
// delay plus the stock intrinsic pin delay), so inputs that switch on
// the same clock edge reach the cell at different times and a
// combinational output can glitch while they settle.  A glitch is the
// whole point: it is the physical effect that a zero-delay RTL
// simulator cannot represent, so a hazard which depends on one is
// invisible to Verilator no matter how the testbench is written.
//
// Clocks are ideal — the ECP5 routes them on a global network whose
// skew is far below the effects under study here.
//
// Delay knobs live in `ecp5_delay_cfg` rather than in a testbench, so
// these models do not depend on what the enclosing testbench is named
// and several gate-level harnesses can share them.
`timescale 1ns/1ps

// Delay policy for the cell models above.  Elaborated as a root module
// (nothing instantiates it) so cells can reach it by name.
//
// Plusargs: +seed=N picks the routing-delay draw, +zero forces every
// delay to 0 as a control — with +zero a hazard that needs a glitch
// must disappear, which is what distinguishes it from a testbench bug.
module ecp5_delay_cfg;
    integer seed = 1;
    reg     zero = 0;
    reg     knobs_read = 0;

    // Cells draw their delays in initial blocks, whose order against
    // this module's own initial block is not defined, so the knobs are
    // read on first use rather than at time 0.
    function void read_knobs;
        if (!knobs_read) begin
            knobs_read = 1;
            if ($value$plusargs("seed=%d", seed)) ;
            if ($test$plusargs("zero")) zero = 1;
        end
    endfunction

    // Routing delay: 0.2 .. 1.2 ns, the span a short intra-slice hop
    // and a long cross-fabric detour occupy on this part.
    function real route_delay;
        begin
            read_knobs();
            route_delay = zero ? 0.0
                               : 0.2 + (($random(seed) & 32'h7fffffff) % 1000) / 1000.0;
        end
    endfunction

    function real intrinsic(input real d);
        begin
            read_knobs();
            intrinsic = zero ? 0.0 : d;
        end
    endfunction
endmodule

module LUT4 #(parameter [15:0] INIT = 16'h0000) (input A, B, C, D, output Z);
    real dA, dB, dC, dD;
    reg a, b, c, d;
    initial begin
        dA = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.141);
        dB = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.275);
        dC = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.379);
        dD = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.379);
        #0.001 a = A; b = B; c = C; d = D;     // constant-tied pins never toggle
    end
    always @(A) a <= #(dA) A;
    always @(B) b <= #(dB) B;
    always @(C) c <= #(dC) C;
    always @(D) d <= #(dD) D;
    wire [7:0] s3 = d ? INIT[15:8] : INIT[7:0];
    wire [3:0] s2 = c ? s3[7:4] : s3[3:0];
    wire [1:0] s1 = b ? s2[3:2] : s2[1:0];
    assign Z = a ? s1[1] : s1[0];
endmodule

module PFUMX (input ALUT, BLUT, C0, output Z);
    real dA, dB, dC;
    reg al, bl, c0;
    initial begin
        dA = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.098);
        dB = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.098);
        dC = ecp5_delay_cfg.route_delay() + ecp5_delay_cfg.intrinsic(0.151);
        #0.001 al = ALUT; bl = BLUT; c0 = C0;
    end
    always @(ALUT) al <= #(dA) ALUT;
    always @(BLUT) bl <= #(dB) BLUT;
    always @(C0)   c0 <= #(dC) C0;
    assign Z = c0 ? al : bl;
endmodule

module TRELLIS_DPR16X4 #(
    parameter WCKMUX = "WCK",
    parameter WREMUX = "WRE",
    parameter [63:0] INITVAL = 64'h0
) (
    input  [3:0] DI,
    input  [3:0] WAD,
    input        WRE,
    input        WCK,
    input  [3:0] RAD,
    output [3:0] DO
);
    reg [3:0] mem [0:15];
    reg [3:0] di, wad, rad;
    reg       wre;
    real dDI [0:3], dWAD [0:3], dRAD [0:3], dWRE;
    integer i;
    initial begin
        for (i = 0; i < 16; i = i + 1) mem[i] = INITVAL[4*i +: 4];
        for (i = 0; i < 4; i = i + 1) begin
            dDI[i]  = ecp5_delay_cfg.route_delay();
            dWAD[i] = ecp5_delay_cfg.route_delay();
            dRAD[i] = ecp5_delay_cfg.route_delay();
        end
        dWRE = ecp5_delay_cfg.route_delay();
        #0.001 di = DI; wad = WAD; rad = RAD; wre = WRE;
    end
    genvar k;
    generate for (k = 0; k < 4; k = k + 1) begin : g_pin
        always @(DI[k])  di[k]  <= #(dDI[k])  DI[k];
        always @(WAD[k]) wad[k] <= #(dWAD[k]) WAD[k];
        always @(RAD[k]) rad[k] <= #(dRAD[k]) RAD[k];
    end endgenerate
    always @(WRE) wre <= #(dWRE) WRE;

    wire muxwck = (WCKMUX == "INV") ? ~WCK : WCK;
    wire muxwre = (WREMUX == "1") ? 1'b1 : (WREMUX == "0") ? 1'b0 :
                  (WREMUX == "INV") ? ~wre : wre;
    always @(posedge muxwck)
        if (muxwre) mem[wad] <= di;
    assign DO = mem[rad];
endmodule

module TRELLIS_FF #(
    parameter GSR = "ENABLED",
    parameter [127:0] CEMUX = "1",
    parameter CLKMUX = "CLK",
    parameter LSRMUX = "LSR",
    parameter SRMODE = "LSR_OVER_CE",
    parameter REGSET = "RESET",
    parameter [127:0] LSRMODE = "LSR"
) (input CLK, LSR, CE, DI, M, output reg Q);
    real dDI, dCE, dLSR, tCQ;
    reg di, ce, lsr;
    initial begin
        dDI  = ecp5_delay_cfg.route_delay();
        dCE  = ecp5_delay_cfg.route_delay();
        dLSR = ecp5_delay_cfg.route_delay();
        tCQ  = ecp5_delay_cfg.intrinsic(0.3);
        #0.001 di = DI; ce = CE; lsr = LSR;
    end
    always @(DI)  di  <= #(dDI)  DI;
    always @(CE)  ce  <= #(dCE)  CE;
    always @(LSR) lsr <= #(dLSR) LSR;

    wire muxce  = (CEMUX == "1" || CEMUX == "1 ") ? 1'b1 :
                  (CEMUX == "0") ? 1'b0 : (CEMUX == "INV") ? ~ce : ce;
    wire muxlsr = (LSRMUX == "INV") ? ~lsr : lsr;
    wire muxclk = (CLKMUX == "INV") ? ~CLK : CLK;
    wire srval  = (REGSET == "SET");
    initial Q = srval;

    generate
        if (SRMODE == "ASYNC") begin : g_async
            always @(posedge muxclk, posedge muxlsr)
                if (muxlsr)     Q <= #(tCQ) srval;
                else if (muxce) Q <= #(tCQ) di;
        end else begin : g_sync
            always @(posedge muxclk)
                if (muxlsr)     Q <= #(tCQ) srval;
                else if (muxce) Q <= #(tCQ) di;
        end
    endgenerate
endmodule
