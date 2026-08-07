// Penumbra USB line-capture instrument (debug tier)
//
// A ring buffer of receive-chain samples for line-level forensics:
// while armed it records the PHY's debug tap once per USB clock, and a
// selected trigger event — an errored or timed-out transaction, a
// bit-stuff violation, or a software force — freezes the buffer after a
// post-trigger tail of one eighth of the depth, leaving the rest as
// pre-trigger context (doc/system/devices/usb-host.md, CAP_* registers).
//
// The recorder runs on the USB clock; readout crosses to the CPU clock
// through the buffer's second port. The contract that makes the plain
// dual-clock BRAM safe is FROZEN: software reads samples only while the
// recorder is stopped, so no word is ever read while it may be written.
// Arm and disarm arrive as toggle events (the CDC start-toggle pattern);
// the trigger selects and force level are quasi-static synchronized
// levels.
//
// A controller reset disarms the recorder but preserves FROZEN, the
// trigger index, and the buffer contents — a capture taken by an
// operating system survives a warm reboot for the boot monitor to read.

// keep_hierarchy: hold this module boundary through synth_ecp5 so the
// debug instrument's cells stay clustered instead of scattering into
// the CPU cone's placement region — the capture is off every
// functional path and must not tax the design's fmax.
(* keep_hierarchy = "yes" *)
module usbhc_capture #(
    parameter int unsigned DEPTH_LOG2 = 12   // 4096 samples, ~68 us at 60 MHz
) (
    // ── USB clock domain: recorder ───────────────────────────────────
    input  logic        i_usb_clk,
    input  logic        i_usb_rst,
    input  logic [15:0] i_sample,        // PHY debug tap, recorded every clock
    input  logic        i_evt_err,       // transaction finished RESULT=ERROR
    input  logic        i_evt_timeout,   // transaction finished RESULT=TIMEOUT
    input  logic        i_evt_stuff,     // bit-stuff violation strobe
    // Control, synchronized in this module
    input  logic        i_arm_toggle,    // CPU-side toggle: arm command
    input  logic        i_disarm_toggle, // CPU-side toggle: disarm command
    input  logic        i_force,         // level: software trigger request
    input  logic [2:0]  i_trig_sel,      // {stuff, timeout, err} enables
    // State toward the register tier (synchronized there or read gated
    // by FROZEN, which is stable once set)
    output logic        o_armed,
    output logic        o_frozen,
    output logic [15:0] o_trig_addr,
    // ── CPU clock domain: readout port ───────────────────────────────
    input  logic        i_clk,
    // Fixed 16-bit index at the register interface; a shallower buffer
    // ignores the excess high bits.
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [15:0] i_rd_addr,
/* verilator lint_on UNUSEDSIGNAL */
    output logic [15:0] o_rd_data
);
    localparam int unsigned DEPTH     = 1 << DEPTH_LOG2;
    localparam int unsigned POST_LEN  = DEPTH / 8;

    // ── Control synchronizers (CPU -> USB) ───────────────────────────
    logic [1:0] arm_sync_q, disarm_sync_q, force_sync_q;
    logic       arm_seen_q, disarm_seen_q;
    logic [2:0] trig_sel_sync_q, trig_sel_q;

    logic arm_cmd, disarm_cmd;
    assign arm_cmd    = (arm_sync_q[1] != arm_seen_q);
    assign disarm_cmd = (disarm_sync_q[1] != disarm_seen_q);

    always_ff @(posedge i_usb_clk) begin
        arm_sync_q      <= {arm_sync_q[0], i_arm_toggle};
        disarm_sync_q   <= {disarm_sync_q[0], i_disarm_toggle};
        force_sync_q    <= {force_sync_q[0], i_force};
        trig_sel_sync_q <= i_trig_sel;
        trig_sel_q      <= trig_sel_sync_q;
        if (i_usb_rst) begin
            arm_seen_q    <= arm_sync_q[1];
            disarm_seen_q <= disarm_sync_q[1];
        end else begin
            if (arm_cmd)    arm_seen_q    <= arm_sync_q[1];
            if (disarm_cmd) disarm_seen_q <= disarm_sync_q[1];
        end
    end

    // ── Recorder ─────────────────────────────────────────────────────
    //
    // S_RUN records and watches the triggers; S_POST records the tail;
    // S_IDLE does not record. FROZEN is state, not a mode: it marks the
    // buffer as holding a completed capture, and only the next arm
    // command clears it. Reset returns to S_IDLE (recording is a live
    // activity) but leaves the completed-capture evidence alone.
    typedef enum logic [1:0] { S_IDLE, S_RUN, S_POST } state_e;
    state_e state_q, state_d;

    logic [DEPTH_LOG2-1:0] wptr_q, wptr_d;
    logic [DEPTH_LOG2-1:0] post_q, post_d;
    logic [DEPTH_LOG2-1:0] trig_addr_q, trig_addr_d;
    logic                  frozen_q, frozen_d;

    logic trigger;
    assign trigger = (trig_sel_q[0] && i_evt_err)     ||
                     (trig_sel_q[1] && i_evt_timeout) ||
                     (trig_sel_q[2] && i_evt_stuff)   ||
                     force_sync_q[1];

    logic recording;
    assign recording = (state_q != S_IDLE);

    always_comb begin
        state_d     = state_q;
        wptr_d      = wptr_q;
        post_d      = post_q;
        trig_addr_d = trig_addr_q;
        frozen_d    = frozen_q;

        if (recording)
            wptr_d = wptr_q + 1'b1;

        case (state_q)
            S_RUN: begin
                if (trigger) begin
                    trig_addr_d = wptr_q;
                    post_d      = DEPTH_LOG2'(POST_LEN);
                    state_d     = S_POST;
                end
            end
            S_POST: begin
                post_d = post_q - 1'b1;
                if (post_q == DEPTH_LOG2'(1)) begin
                    frozen_d = 1'b1;
                    state_d  = S_IDLE;
                end
            end
            default: ;   // S_IDLE: hold
        endcase

        // Commands outrank the trigger walk: an arm restarts a fresh
        // capture from any state; a disarm abandons one without
        // declaring its buffer complete.
        if (arm_cmd) begin
            frozen_d = 1'b0;
            state_d  = S_RUN;
        end else if (disarm_cmd) begin
            state_d = S_IDLE;
        end
    end

    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst) begin
            state_q <= S_IDLE;
            wptr_q  <= '0;
            post_q  <= '0;
            // frozen_q / trig_addr_q deliberately unreset: a completed
            // capture survives a warm reboot for post-mortem readout.
        end else begin
            state_q <= state_d;
            wptr_q  <= wptr_d;
            post_q  <= post_d;
        end
        frozen_q    <= i_usb_rst ? frozen_q : frozen_d;
        trig_addr_q <= i_usb_rst ? trig_addr_q : trig_addr_d;
    end

    // Registered at the recorder's edge so the values leave this module
    // from flops; the register tier re-registers them on the CPU clock.
    // Both are quasi-static when read (FROZEN gates TRIG_ADDR).
    logic armed_out_q, frozen_out_q;
    logic [15:0] trig_addr_out_q;
    always_ff @(posedge i_usb_clk) begin
        armed_out_q     <= recording;
        frozen_out_q    <= frozen_q;
        trig_addr_out_q <= 16'(trig_addr_q);
    end
    assign o_armed     = armed_out_q;
    assign o_frozen    = frozen_out_q;
    assign o_trig_addr = trig_addr_out_q;

    // ── Sample buffer: dual-clock BRAM ───────────────────────────────
    //
    // Written every USB clock while recording; read from the CPU domain.
    // Simultaneous access to one word never happens by contract: readout
    // is only meaningful under FROZEN, when the recorder is stopped.
    logic [15:0] mem [DEPTH];

    always_ff @(posedge i_usb_clk) begin
        if (recording)
            mem[wptr_q] <= i_sample;
    end

    always_ff @(posedge i_clk)
        o_rd_data <= mem[i_rd_addr[DEPTH_LOG2-1:0]];

    // The post-trigger countdown only runs while recording.
    assert property (@(posedge i_usb_clk) disable iff (i_usb_rst)
        ((state_q == S_POST) |-> recording))
        else $error("usbhc_capture: post-trigger count outside recording");
endmodule
