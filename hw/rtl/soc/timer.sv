// Penumbra Programmable Interval Timer — sysreg device for periodic interrupts
//
// Sysreg device 7 (SYSDEV_TIMER). 16-bit countdown timer that ticks at a
// fixed hardware frequency independent of CPU clock. On underflow the
// counter optionally auto-reloads from the reload register and/or asserts
// an interrupt request.
//
// The tick input (i_tick) comes from a prescaler or external oscillator,
// synchronised to i_clk internally via a two-FF synchroniser.
//
// Register map:
//   0  TMFREQ   — Tick frequency in Hz (read-only, hardwired parameter)
//   1  TMCR     — Control: [0]=TICK_EN, [1]=IRQ_EN, [2]=AUTOLOAD
//   2  TMCOUNT  — Current 16-bit counter value (counts down each tick)
//   3  TMRELOAD — 16-bit reload value (copied to COUNT on underflow)
//   4  TMSTATUS — [0]=UDF (underflow flag), write-1-to-clear
//
// Behaviour:
//   - When TICK_EN=1, COUNT decrements by 1 on each synchronised tick edge.
//   - When COUNT reaches 0 and the next tick arrives:
//       • UDF flag in TMSTATUS is set.
//       • If AUTOLOAD=1, COUNT is loaded from TMRELOAD.
//       • If AUTOLOAD=0, TICK_EN is cleared (one-shot).
//   - o_irq is asserted when UDF=1 and IRQ_EN=1 (level-triggered).
//   - Writing TMCOUNT while running updates the counter immediately.
//   - Writing TMRELOAD does not affect the running counter.
//
// Discrete 74xx: 2× '574 (16-bit counter), 3× flip-flop (CR bits),
// 1× flip-flop (UDF), 2× flip-flop (synchroniser), comparator + gates.

// verilator lint_off UNUSEDSIGNAL

module timer
    import penumbra_pkg::*;
#(
    parameter logic [31:0] TICK_FREQ_HZ = 32'd1_000_000  // Default 1 MHz
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Tick input (may be in a different clock domain) ────
    input  logic        i_tick,

    // ── Sysreg interface ──────────────────────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata,

    // ── Interrupt output ──────────────────────────────────
    output logic        o_irq
);

    // ── Tick synchroniser (two-FF, clock domain crossing) ──
    logic tick_sync1, tick_sync2, tick_prev, tick_edge;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tick_sync1 <= 1'b0;
            tick_sync2 <= 1'b0;
            tick_prev  <= 1'b0;
        end else begin
            tick_sync1 <= i_tick;
            tick_sync2 <= tick_sync1;
            tick_prev  <= tick_sync2;
        end
    end

    // Detect rising edge of synchronised tick
    assign tick_edge = tick_sync2 & ~tick_prev;

    // ── Control register bits ─────────────────────────────
    logic tick_en;      // TMCR[0]: enable counting
    logic irq_en;       // TMCR[1]: enable interrupt output
    logic autoload;     // TMCR[2]: auto-reload on underflow

    // ── Counter and reload ────────────────────────────────
    logic [15:0] count;
    logic [15:0] reload;

    // ── Status ────────────────────────────────────────────
    logic udf;          // Underflow flag

    // ── Underflow detection ───────────────────────────────
    logic underflow;
    assign underflow = tick_en & tick_edge & (count == 16'd0);

    // ── Sequential logic ──────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tick_en  <= 1'b0;
            irq_en   <= 1'b0;
            autoload <= 1'b0;
            count    <= 16'd0;
            reload   <= 16'd0;
            udf      <= 1'b0;
        end else begin
            // ── Sysreg writes ─────────────────────────────
            if (i_sys_we) begin
                case (i_sys_reg)
                    SYSREG_TM_CR: begin
                        tick_en  <= i_sys_wdata[0];
                        irq_en   <= i_sys_wdata[1];
                        autoload <= i_sys_wdata[2];
                    end
                    SYSREG_TM_COUNT:
                        count <= i_sys_wdata[15:0];
                    SYSREG_TM_RELOAD:
                        reload <= i_sys_wdata[15:0];
                    SYSREG_TM_STATUS:
                        // Write-1-to-clear: clear UDF if bit 0 written as 1
                        if (i_sys_wdata[0])
                            udf <= 1'b0;
                    default: ;
                endcase
            end

            // ── Timer tick logic (after sysreg write so writes take
            //    effect same cycle, but underflow overrides UDF clear) ──
            if (underflow) begin
                udf <= 1'b1;
                if (autoload)
                    count <= reload;
                else
                    tick_en <= 1'b0;    // One-shot: stop
            end else if (tick_en & tick_edge) begin
                count <= count - 16'd1;
            end
        end
    end

    // ── IRQ output: level-triggered ───────────────────────
    assign o_irq = udf & irq_en;

    // ── Read mux ──────────────────────────────────────────
    always_comb begin
        case (i_sys_reg)
            SYSREG_TM_FREQ:   o_sys_rdata = TICK_FREQ_HZ;
            SYSREG_TM_CR:     o_sys_rdata = {29'b0, autoload, irq_en, tick_en};
            SYSREG_TM_COUNT:  o_sys_rdata = {16'b0, count};
            SYSREG_TM_RELOAD: o_sys_rdata = {16'b0, reload};
            SYSREG_TM_STATUS: o_sys_rdata = {31'b0, udf};
            default:           o_sys_rdata = 32'b0;
        endcase
    end

endmodule
