// SDRAM clock-domain-crossing bridge — async req/ack handshake.
//
// Sits between `sdram_bus_adapter` (system clock) and `sdram_ctrl`
// (SDRAM clock).  Single outstanding request.  Standard 4-phase
// handshake using toggle synchronizers in each direction.
//
//   sys side                              sdram side
//   ────────                              ──────────
//   adapter ─req──► [latch payload,      [2FF sync on req_tog,
//                    toggle req_tog,      edge-detect, sample
//                    sys_busy=1,          payload from sys regs,
//                    pulse o_req_ready]   drive ctrl req] ──► ctrl
//                                          │
//                                          │ (controller runs the
//                                          │  ACT/RW/RECOVER)
//                                          ▼
//   adapter ◄─done─[2FF sync on done_tog,◄[latch rsp_data, toggle
//                   edge-detect, pulse    done_tog]
//                   o_done/o_rsp_valid,
//                   sys_busy=0]
//
// CDC discipline:
//   • Wide payload (we/addr/wdata/byte_en) is *not* synchronized
//     per-bit.  It is held quasi-statically in sys-domain registers
//     (sys_busy=1) and sampled by the sdram side only AFTER the
//     synchronized req-toggle has signalled "data is stable".  This
//     is the standard "data-before-valid" CDC pattern.
//   • The 1-bit toggle line in each direction goes through a 2-FF
//     synchronizer plus a delay register.  Edge is detected as
//     `tog_sync2 != tog_sync_dly`.
//   • rsp_data is sampled across the boundary the same way: latched
//     once on the sdram side at the moment done_tog is toggled, then
//     read directly by the sys side after the toggle propagates.
//     Stable for many sys cycles since the sdram side stays in
//     SD_IDLE until the next request edge arrives.
//
// Latency added by the bridge (in addition to controller timings):
//   • Request:  ~3 sdram cycles (sync + edge detect + state advance)
//   • Done:     ~3 sys cycles
// Negligible against T_RCD + CL + T_RP + 2 burst beats at 100 MHz.
//
// The bridge's depth is fixed at one outstanding request.  Going
// deeper (async FIFO) is a future change; the boundary keeps the
// same shape.

module sdram_cdc (
    // ── Sys-domain (adapter side) ────────────────────────────
    input  logic        i_sys_clk,
    input  logic        i_sys_rst,

    // Request from adapter
    input  logic        i_sys_req_valid,
    input  logic        i_sys_req_we,
    input  logic [31:0] i_sys_req_addr,
    input  logic [31:0] i_sys_req_wdata,
    input  logic [3:0]  i_sys_req_byte_en,
    output logic        o_sys_req_ready,    // one-cycle pulse

    // Response to adapter
    output logic        o_sys_rsp_valid,    // one-cycle pulse (reads)
    output logic [31:0] o_sys_rsp_data,
    input  logic        i_sys_rsp_ready,    // unused; adapter is always ready
    output logic        o_sys_done,         // one-cycle pulse

    // ── SDRAM-domain (controller side) ───────────────────────
    input  logic        i_sd_clk,
    input  logic        i_sd_rst,

    // Request to controller
    output logic        o_sd_req_valid,
    output logic        o_sd_req_we,
    output logic [31:0] o_sd_req_addr,
    output logic [31:0] o_sd_req_wdata,
    output logic [3:0]  o_sd_req_byte_en,
    input  logic        i_sd_req_ready,

    // Response from controller
    input  logic        i_sd_rsp_valid,
    input  logic [31:0] i_sd_rsp_data,
    output logic        o_sd_rsp_ready,     // tied high; controller ignores
    input  logic        i_sd_done
);

    // ── i_sys_rsp_ready is part of the protocol but unused today ──
    /* verilator lint_off UNUSEDSIGNAL */
    wire _unused = &{1'b0, i_sys_rsp_ready};
    /* verilator lint_on UNUSEDSIGNAL */

    // ─────────────────────────────────────────────────────────
    // Sys-domain state
    // ─────────────────────────────────────────────────────────
    logic        sys_busy;
    logic        req_tog_sys;       // toggles when launching a new request

    // Quasi-static payload registers; held stable while sys_busy = 1.
    logic        req_we_r;
    logic [31:0] req_addr_r;
    logic [31:0] req_wdata_r;
    logic [3:0]  req_byte_en_r;

    // 2-FF synchronizer + delay reg for done_tog from SDRAM domain.
    logic done_tog_sync1, done_tog_sync2, done_tog_sync_dly;

    // Latched copy of rsp_data presented to the adapter.
    logic [31:0] rsp_data_sys;

    // ─────────────────────────────────────────────────────────
    // SDRAM-domain state
    // ─────────────────────────────────────────────────────────
    logic req_tog_sync1, req_tog_sync2, req_tog_sync_dly;
    logic done_tog_sd;
    logic [31:0] rsp_data_sd;

    typedef enum logic [1:0] {
        SD_IDLE,
        SD_DRIVE_REQ,
        SD_WAIT_DONE
    } sd_state_t;
    sd_state_t sd_state;

    // ─────────────────────────────────────────────────────────
    // Sys side: synchronize done_tog from SDRAM domain
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_sys_clk) begin
        if (i_sys_rst) begin
            done_tog_sync1   <= 1'b0;
            done_tog_sync2   <= 1'b0;
            done_tog_sync_dly<= 1'b0;
        end else begin
            done_tog_sync1   <= done_tog_sd;
            done_tog_sync2   <= done_tog_sync1;
            done_tog_sync_dly<= done_tog_sync2;
        end
    end

    wire done_pulse_sys = done_tog_sync2 ^ done_tog_sync_dly;

    // ─────────────────────────────────────────────────────────
    // Sys side: launch + done handling
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_sys_clk) begin
        if (i_sys_rst) begin
            sys_busy        <= 1'b0;
            req_tog_sys     <= 1'b0;
            req_we_r        <= 1'b0;
            req_addr_r      <= '0;
            req_wdata_r     <= '0;
            req_byte_en_r   <= '0;
            rsp_data_sys    <= '0;
            o_sys_req_ready <= 1'b0;
            o_sys_done      <= 1'b0;
            o_sys_rsp_valid <= 1'b0;
        end else begin
            // Default: deassert one-cycle pulses.
            o_sys_req_ready <= 1'b0;
            o_sys_done      <= 1'b0;
            o_sys_rsp_valid <= 1'b0;

            // Done from SDRAM side: toggle synchronizer just saw an
            // edge.  Pulse done/rsp_valid back to the adapter,
            // sample the SDRAM-domain rsp_data (held stable since
            // SDRAM's i_done cycle), and clear sys_busy.
            if (sys_busy && done_pulse_sys) begin
                o_sys_done      <= 1'b1;
                o_sys_rsp_valid <= 1'b1;
                rsp_data_sys    <= rsp_data_sd;
                sys_busy        <= 1'b0;
            end

            // When the adapter presents a fresh request and the
            // bridge is idle, capture the payload, toggle req_tog
            // so the SDRAM side detects an edge, set sys_busy, and
            // pulse req_ready back to the adapter.
            if (!sys_busy && i_sys_req_valid) begin
                // Latch payload
                req_we_r      <= i_sys_req_we;
                req_addr_r    <= i_sys_req_addr;
                req_wdata_r   <= i_sys_req_wdata;
                req_byte_en_r <= i_sys_req_byte_en;

                // Trigger the SDRAM clock domain side: flip the
                // toggle so every request produces an edge, regardless
                // of the previous state.
                req_tog_sys     <= ~req_tog_sys;
                // Mark busy
                sys_busy        <= 1'b1;
                // Notify the bus adapter
                o_sys_req_ready <= 1'b1;
            end
        end
    end

    assign o_sys_rsp_data = rsp_data_sys;

    // ─────────────────────────────────────────────────────────
    // SDRAM side: synchronize req_tog from sys domain
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_sd_clk) begin
        if (i_sd_rst) begin
            req_tog_sync1    <= 1'b0;
            req_tog_sync2    <= 1'b0;
            req_tog_sync_dly <= 1'b0;
        end else begin
            req_tog_sync1    <= req_tog_sys;
            req_tog_sync2    <= req_tog_sync1;
            req_tog_sync_dly <= req_tog_sync2;
        end
    end

    wire req_pulse_sd = req_tog_sync2 ^ req_tog_sync_dly;

    // ─────────────────────────────────────────────────────────
    // SDRAM side: drive controller, capture rsp, signal done
    // ─────────────────────────────────────────────────────────
    assign o_sd_rsp_ready = 1'b1;

    always_ff @(posedge i_sd_clk) begin
        if (i_sd_rst) begin
            sd_state         <= SD_IDLE;
            done_tog_sd      <= 1'b0;
            rsp_data_sd      <= '0;
            o_sd_req_valid   <= 1'b0;
            o_sd_req_we      <= 1'b0;
            o_sd_req_addr    <= '0;
            o_sd_req_wdata   <= '0;
            o_sd_req_byte_en <= '0;
        end else begin
            case (sd_state)
                SD_IDLE: begin
                    if (req_pulse_sd) begin
                        // Sys-domain payload registers are held stable
                        // (sys_busy=1) until our done_tog round-trip
                        // returns — safe to sample directly.
                        o_sd_req_we      <= req_we_r;
                        o_sd_req_addr    <= req_addr_r;
                        o_sd_req_wdata   <= req_wdata_r;
                        o_sd_req_byte_en <= req_byte_en_r;
                        o_sd_req_valid   <= 1'b1;
                        sd_state         <= SD_DRIVE_REQ;
                    end
                end

                SD_DRIVE_REQ: begin
                    if (i_sd_req_ready) begin
                        o_sd_req_valid <= 1'b0;
                        sd_state       <= SD_WAIT_DONE;
                    end
                end

                SD_WAIT_DONE: begin
                    if (i_sd_done) begin
                        // Sample rsp_data only when it is signalled
                        // valid (reads).  For writes the controller
                        // pulses i_sd_done without i_sd_rsp_valid; the
                        // adapter ignores rsp on writes anyway, so
                        // leaving rsp_data_sd at its previous value is
                        // fine.  Toggling done_tog_sd in the same
                        // cycle is safe — the 2-FF sync on the sys
                        // side observes data after the toggle has
                        // propagated through both flops.
                        if (i_sd_rsp_valid) rsp_data_sd <= i_sd_rsp_data;
                        done_tog_sd <= ~done_tog_sd;
                        sd_state    <= SD_IDLE;
                    end
                end

                default: sd_state <= SD_IDLE;
            endcase
        end
    end

endmodule
