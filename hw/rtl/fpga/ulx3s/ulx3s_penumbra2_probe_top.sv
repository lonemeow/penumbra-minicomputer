// ULX3S Board Top (Penumbra/2 probe) — gen2 machine timing probe.
//
// A synthesis instrument, not a usable machine: machine_penumbra2 (core +
// MMU + VIPT L1 I/D caches + I/D arbiter + fill sequencer + L2) with a
// small BRAM bus memory behind the board pins, so any gen2 RTL change can
// be checked for fmax movement
// (make timing BOARD=ulx3s CORE=penumbra2 VARIANT=probe) before the real
// board fabric exists. This is the first build that puts the IF2
// tag-compare / way-mux path and the L1<->L2 layer in front of nextpnr —
// the timing data Decision 11's 4-way-vs-2-way choice is gated on. Static
// timing analysis is the product; runtime behavior is not — the memory is
// zero-initialized (INIT_FILE=""), so from reset the machine free-runs on
// whatever an all-zero instruction word decodes to.
//
// The LED reduction at the bottom is load-bearing: it is the only consumer
// of the machine's terminal outputs, so without it synthesis would prune
// the design and the timing report would measure nothing. The buttons
// drive the IRQ inputs for the same reason — they keep the
// interrupt-recognition logic in the timing cone. The bus loop
// (machine -> BRAM memory -> machine) keeps the whole memory system alive.
//
// Clocking is the 25 MHz crystal directly (no PLL): one clean clock
// domain, directly comparable fmax numbers from build to build.

module ulx3s_penumbra2_probe_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    // btn[1] = FIRE1 manual reset; btn[2]/btn[3] drive the IRQ lines.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en      // LOW = hold ESP32 in reset
);
    import penumbra2_pkg::*;

    // The probe has no UART — keep the ESP32 quiet.
    assign wifi_en = 1'b0;

    // ── Reset: power-on counter + btn[1] (FIRE1) manual reset ────
    // 2^19 / 25 MHz ≈ 21 ms, same envelope as the gen1 board top.
    logic btn1_sync1, btn1_sync2;
    always_ff @(posedge clk_25mhz) begin
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
    end

    // Init value at sim-time is harmless and ensures rst_cnt starts
    // at 0; the always_ff covers post-reset behaviour.
    /* verilator lint_off PROCASSINIT */
    logic [18:0] rst_cnt = '0;
    /* verilator lint_on PROCASSINIT */
    logic rst;

    always_ff @(posedge clk_25mhz) begin
        if (btn1_sync2)
            rst_cnt <= '0;
        else if (!rst_cnt[18])
            rst_cnt <= rst_cnt + 1;
    end
    assign rst = !rst_cnt[18];

    // ── IRQ lines from buttons (2-FF synchronized) ───────────────
    logic [1:0] irq_sync1, irq_sync2;
    always_ff @(posedge clk_25mhz) begin
        irq_sync1 <= {btn[3], btn[2]};
        irq_sync2 <= irq_sync1;
    end

    // ── The DUT: the gen2 machine + a BRAM bus memory ────────────
    logic [SB_IDX_W-1:0] commit_idx;
    logic [31:0]         commit_data;
    logic                commit_we;
    logic                retire_valid;
    logic [OPC_W-1:0]    retire_op_class;
    logic                prog_end;

    logic [31:0] bus_addr, bus_wdata, bus_rdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re, bus_we, bus_busy;

    machine_penumbra2 u_machine (
        .i_clk             (clk_25mhz),
        .i_rst             (rst),
        .i_irq             (irq_sync2[0]),
        .i_timer_irq       (irq_sync2[1]),
        .o_bus_addr        (bus_addr),
        .o_bus_wdata       (bus_wdata),
        .o_bus_byte_en     (bus_byte_en),
        .o_bus_re          (bus_re),
        .o_bus_we          (bus_we),
        .i_bus_rdata       (bus_rdata),
        .i_bus_busy        (bus_busy),
        .o_commit_idx      (commit_idx),
        .o_commit_data     (commit_data),
        .o_commit_we       (commit_we),
        .o_retire_valid    (retire_valid),
        .o_retire_op_class (retire_op_class),
        .o_prog_end        (prog_end)
    );

    unified_bus_mem #(.INIT_FILE("")) u_mem (
        .i_clk     (clk_25mhz),
        .i_rst     (rst),
        .i_addr    (bus_addr),
        .i_wdata   (bus_wdata),
        .i_byte_en (bus_byte_en),
        .i_re      (bus_re),
        .i_we      (bus_we),
        .o_rdata   (bus_rdata),
        .o_busy    (bus_busy)
    );

    // ── Keep-alive: fold the machine's outputs onto the LEDs ─────
    // Every terminal output must feed this register so no part of the
    // commit/retire cone is dead at synthesis.
    logic [7:0] led_r;
    always_ff @(posedge clk_25mhz) begin
        led_r <= {7'b0,
                  (^commit_idx)
                  ^ (^commit_data)
                  ^ commit_we
                  ^ retire_valid
                  ^ (^retire_op_class)
                  ^ prog_end};
    end
    assign led = led_r;

endmodule
