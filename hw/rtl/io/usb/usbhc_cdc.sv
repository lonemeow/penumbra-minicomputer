// Penumbra USB host controller clock-domain crossing (CPU <-> USB)
//
// Everything that moves between the register tier (CPU clock) and the
// MAC (the seam's 60 MHz clock) crosses here, each by the discipline its
// shape demands — the same conventions as sdram_cdc:
//
//   * The packet data buffer is a true dual-port memory, one port per
//     domain: no payload byte ever crosses a synchronizer. Concurrent
//     access is excluded by ownership, not arbitration — software fills
//     or reads DATA only while no transaction runs, the MAC touches it
//     only while one does, and the START/DONE handshake is the fence.
//   * START and DONE are toggle-and-sync pulse crossings. Their payloads
//     ride data-before-toggle: the request fields (TOKEN, LENGTH) are
//     held stable by the register tier from START until DONE, and the
//     result fields are latched USB-side before the DONE toggle flips,
//     so the far side always samples settled values.
//   * PORT_CHANGE and SOF are payload-less toggle crossings; their
//     minimum spacing (a debounce window, a frame) dwarfs the
//     synchronizer latency, so no event can be swallowed.
//   * The PORT_CTRL levels cross CPU->USB and the port-status levels
//     USB->CPU through per-bit 2-FF synchronizers. Multi-bit status
//     fields (SPEED, LINE) may read mixed for a cycle mid-change; every
//     consumer is notified by PORT_CHANGE and re-reads settled values.
//   * FRAME crosses as Gray code — an increment-by-one counter is the
//     one multi-bit value a plain synchronizer handles exactly.

module usbhc_cdc #(
    parameter int BUF_BYTES = 64
) (
    // ── CPU clock domain (register tier) ─────────────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
    // Data buffer, CPU byte port (synchronous read, 1-cycle). The
    // address matches the MAC's 7-bit port shape; bits above the
    // configured capacity are unused.
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0]  i_cbuf_addr,
/* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]  i_cbuf_wdata,
    input  logic        i_cbuf_we,
    output logic [7:0]  o_cbuf_rdata,
    // Transaction launch; the request fields must hold until o_done
    input  logic        i_start,        // one-cycle pulse
    // Completion toward the register tier
    output logic        o_done,         // one-cycle pulse; results settled
    output logic [2:0]  o_result,
    output logic [6:0]  o_rxlen,
    output logic        o_rxtoggle,
    // Events toward the register tier
    output logic        o_port_change,  // one-cycle pulse
    output logic        o_sof,          // one-cycle pulse
    // Port status view (synced levels) + frame counter
    output logic        o_connect,
    output logic        o_enabled,
    output logic        o_reset_active,
    output logic        o_suspended,
    output logic [1:0]  o_speed,
    output logic [1:0]  o_line,
    output logic [10:0] o_frame,
    // PORT_CTRL levels from the register tier
    input  logic        i_run,
    input  logic        i_power,
    input  logic        i_reset_port,
    input  logic        i_suspend,
    input  logic        i_resume,

    // ── USB clock domain (MAC) ───────────────────────────────────────
    input  logic        i_usb_clk,
    input  logic        i_usb_rst,
    // Data buffer, USB byte port (synchronous read, 1-cycle)
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0]  i_ubuf_addr,
/* verilator lint_on UNUSEDSIGNAL */
    input  logic [7:0]  i_ubuf_wdata,
    input  logic        i_ubuf_we,
    output logic [7:0]  o_ubuf_rdata,
    // Transaction launch toward the MAC
    output logic        o_u_start,      // one-cycle pulse
    // Completion from the MAC; fields sampled on i_u_done
    input  logic        i_u_done,
    input  logic [2:0]  i_u_result,
    input  logic [6:0]  i_u_rxlen,
    input  logic        i_u_rxtoggle,
    // Events from the MAC
    input  logic        i_u_port_change,
    input  logic        i_u_sof,
    // Port status levels + frame counter from the MAC
    input  logic        i_u_connect,
    input  logic        i_u_enabled,
    input  logic        i_u_reset_active,
    input  logic        i_u_suspended,
    input  logic [1:0]  i_u_speed,
    input  logic [1:0]  i_u_line,
    input  logic [10:0] i_u_frame,
    // PORT_CTRL levels toward the MAC (synced)
    output logic        o_u_run,
    output logic        o_u_power,
    output logic        o_u_reset_port,
    output logic        o_u_suspend,
    output logic        o_u_resume
);

    // ── Packet data buffer: one true dual-port memory ────────────────
    // Two write ports on one array is exactly the dual-clock BRAM shape;
    // the START/DONE ownership fence excludes same-address collisions.
    // The byte-address ports are 7 bits to match the MAC's; the index
    // uses what the capacity needs.
    localparam int AW = $clog2(BUF_BYTES);
    logic [AW-1:0] cbuf_idx, ubuf_idx;
    assign cbuf_idx = i_cbuf_addr[AW-1:0];
    assign ubuf_idx = i_ubuf_addr[AW-1:0];

    /* verilator lint_off MULTIDRIVEN */
    logic [7:0] buf_mem [0:BUF_BYTES-1];
    /* verilator lint_on MULTIDRIVEN */

    always_ff @(posedge i_clk) begin
        if (i_cbuf_we)
            buf_mem[cbuf_idx] <= i_cbuf_wdata;
        o_cbuf_rdata <= buf_mem[cbuf_idx];
    end

    always_ff @(posedge i_usb_clk) begin
        if (i_ubuf_we)
            buf_mem[ubuf_idx] <= i_ubuf_wdata;
        o_ubuf_rdata <= buf_mem[ubuf_idx];
    end

    // ── START: CPU -> USB toggle crossing ────────────────────────────
    logic start_tog_q;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            start_tog_q <= 1'b0;
        else if (i_start)
            start_tog_q <= ~start_tog_q;
    end

    logic [2:0] u_start_sync_q;   // 2-FF sync + edge-detect stage
    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst)
            u_start_sync_q <= 3'b000;
        else
            u_start_sync_q <= {u_start_sync_q[1:0], start_tog_q};
    end
    assign o_u_start = u_start_sync_q[2] ^ u_start_sync_q[1];

    // ── DONE: USB -> CPU toggle crossing, results ride ahead ─────────
    logic       done_tog_q;
    logic [2:0] done_result_q;
    logic [6:0] done_rxlen_q;
    logic       done_rxtoggle_q;
    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst) begin
            done_tog_q      <= 1'b0;
            done_result_q   <= 3'd0;
            done_rxlen_q    <= 7'd0;
            done_rxtoggle_q <= 1'b0;
        end else if (i_u_done) begin
            // The payload and the toggle flip on the same edge; the CPU
            // side sees the toggle two-plus cycles later, by which time
            // the payload has long settled.
            done_result_q   <= i_u_result;
            done_rxlen_q    <= i_u_rxlen;
            done_rxtoggle_q <= i_u_rxtoggle;
            done_tog_q      <= ~done_tog_q;
        end
    end

    logic [2:0] done_sync_q;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            done_sync_q <= 3'b000;
        else
            done_sync_q <= {done_sync_q[1:0], done_tog_q};
    end
    assign o_done     = done_sync_q[2] ^ done_sync_q[1];
    assign o_result   = done_result_q;
    assign o_rxlen    = done_rxlen_q;
    assign o_rxtoggle = done_rxtoggle_q;

    // ── Events: payload-less toggle crossings, USB -> CPU ────────────
    logic pchg_tog_q, sof_tog_q;
    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst) begin
            pchg_tog_q <= 1'b0;
            sof_tog_q  <= 1'b0;
        end else begin
            if (i_u_port_change)
                pchg_tog_q <= ~pchg_tog_q;
            if (i_u_sof)
                sof_tog_q <= ~sof_tog_q;
        end
    end

    logic [2:0] pchg_sync_q, sof_sync_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pchg_sync_q <= 3'b000;
            sof_sync_q  <= 3'b000;
        end else begin
            pchg_sync_q <= {pchg_sync_q[1:0], pchg_tog_q};
            sof_sync_q  <= {sof_sync_q[1:0], sof_tog_q};
        end
    end
    assign o_port_change = pchg_sync_q[2] ^ pchg_sync_q[1];
    assign o_sof         = sof_sync_q[2] ^ sof_sync_q[1];

    // ── Levels: per-bit 2-FF synchronizers, both directions ──────────
    localparam int CTRL_W = 5;
    logic [CTRL_W-1:0] ctrl_cpu;
    logic [CTRL_W-1:0] ctrl_sync_q [0:1];
    assign ctrl_cpu = {i_resume, i_suspend, i_reset_port, i_power, i_run};
    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst) begin
            ctrl_sync_q[0] <= '0;
            ctrl_sync_q[1] <= '0;
        end else begin
            ctrl_sync_q[0] <= ctrl_cpu;
            ctrl_sync_q[1] <= ctrl_sync_q[0];
        end
    end
    assign {o_u_resume, o_u_suspend, o_u_reset_port, o_u_power, o_u_run} =
        ctrl_sync_q[1];

    localparam int STAT_W = 8;
    logic [STAT_W-1:0] stat_usb;
    logic [STAT_W-1:0] stat_sync_q [0:1];
    assign stat_usb = {i_u_line, i_u_speed, i_u_suspended, i_u_reset_active,
                       i_u_enabled, i_u_connect};
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            stat_sync_q[0] <= '0;
            stat_sync_q[1] <= '0;
        end else begin
            stat_sync_q[0] <= stat_usb;
            stat_sync_q[1] <= stat_sync_q[0];
        end
    end
    assign {o_line, o_speed, o_suspended, o_reset_active, o_enabled,
            o_connect} = stat_sync_q[1];

    // ── FRAME: Gray-coded counter crossing, USB -> CPU ───────────────
    logic [10:0] frame_gray_q;
    always_ff @(posedge i_usb_clk) begin
        if (i_usb_rst)
            frame_gray_q <= '0;
        else
            frame_gray_q <= i_u_frame ^ (i_u_frame >> 1);
    end

    logic [10:0] frame_sync_q [0:1];
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            frame_sync_q[0] <= '0;
            frame_sync_q[1] <= '0;
        end else begin
            frame_sync_q[0] <= frame_gray_q;
            frame_sync_q[1] <= frame_sync_q[0];
        end
    end

    // Gray decode: each binary bit is the XOR-fold of the gray bits at
    // and above it.
    always_comb begin
        for (int i = 0; i <= 10; i++)
            o_frame[i] = ^(frame_sync_q[1] >> i);
    end
endmodule
