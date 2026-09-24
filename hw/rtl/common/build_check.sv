// build_check — refuse to build when an elaboration-time condition fails
//
// For configuration a build must reject rather than ship: a PLL with no
// legal divider set, a parameter combination the logic cannot honor.
// Neither an `initial assert` nor a generate-block `$error` can carry such
// a check. sv2v, which feeds every synthesis flow, deletes the assertion
// (Verilog-2005 has none) and turns the severity task into a message that
// synthesis prints and then carries on from.
//
// Instantiate once per condition; the module has no ports:
//
//     build_check #(
//         .COND (PLL.valid),
//         .MSG  ("ecp5_pll: no legal PLL config for CPU_HZ")
//     ) u_check_pll_valid ();
//
// COND must be an elaboration-time constant. MSG is a plain string, not a
// format: a parameter carries no format arguments, so a message names the
// offending parameter rather than printing its value.
//
// COND defaults to passing because yosys elaborates every module with its
// default parameters as it reads the file: a failing default would stop
// every build that merely lists this file.
//
// A failing check instantiates build_check_failed, which no file defines.
// Every tool treats the missing module as fatal (Verilator MODMISSING,
// yosys hierarchy -check, Vivado [Synth 8-439]), whatever its lint or
// warning settings, and the name tells a reader what stopped the build.
// Each tool also prints MSG during elaboration.

module build_check #(
    parameter bit    COND = 1'b1,
    parameter string MSG  = ""
) ();

    if (!COND) begin : g_failed
        // Under Verilator, $error is elaborated natively and names the
        // failing instance. Everything else reads sv2v output, where neither
        // severity task survives intact: $fatal becomes a $display plus a
        // $finish that yosys executes before printing anything, and $error
        // a $display whose message argument Vivado drops. A plain $display
        // with the format in its first argument reaches both logs.
`ifdef VERILATOR
        $error("%s", MSG);
`else
        initial $display("build_check: %s", MSG);
`endif

        build_check_failed u_stop ();
    end

endmodule
