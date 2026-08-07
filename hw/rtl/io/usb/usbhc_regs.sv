// Penumbra USB host controller register tier (CPU clock domain)
//
// The bus-facing half of the CLASS_USBHC minimum protocol
// (doc/system/devices/usb-host.md): word-strided registers, the sticky
// W1C interrupt sources with their mask, and the DATA window onto the
// dual-clock packet buffer. Everything USB-side arrives through
// usbhc_cdc — this module never sees the USB clock.
//
// DATA is word-strided at the bus but byte-wide at the buffer, so a
// DATA access runs a short byte sequence against the CDC's CPU port
// under bus stall: four writes for a stored word, four pipelined reads
// for a loaded one. Ordinary registers answer with the 1-cycle
// registered read every bus device presents (the access_pending busy
// idiom); the sequencer just stretches the same stall.
//
// The transaction request fields (TOKEN, LENGTH) are register outputs
// held stable from the START pulse until DONE — the stability the CDC's
// data-before-toggle discipline relies on. Software's contract ("launch
// only while no transaction runs") is the usual register-tier rule;
// hardware does not police a START while busy.

module usbhc_regs
    import penumbra_pkg::*;
#(
    parameter int BUF_BYTES = 64,
    parameter int unsigned CAP_DEPTH_LOG2 = 12
) (
    input  logic        i_clk,
    input  logic        i_rst,
    // Bus interface (behind autoconfig_dev); word-strided decode uses
    // the window-offset bits only
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,
/* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_irq,
    // PHY capability ceiling {hs, fs, ls} — constant, folded into CAP;
    // the hs bit has no CAP field in the FS/LS minimum
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [2:0]  i_caps,
/* verilator lint_on UNUSEDSIGNAL */
    // usbhc_cdc, CPU side: data buffer byte port
    output logic [6:0]  o_cbuf_addr,
    output logic [7:0]  o_cbuf_wdata,
    output logic        o_cbuf_we,
    input  logic [7:0]  i_cbuf_rdata,
    // usbhc_cdc, CPU side: transaction handshake + results
    output logic        o_start,
    input  logic        i_done,
    input  logic [2:0]  i_result,
    input  logic [6:0]  i_rxlen,
    input  logic        i_rxtoggle,
    // usbhc_cdc, CPU side: events
    input  logic        i_port_change,
    input  logic        i_sof,
    // usbhc_cdc, CPU side: port status view + frame
    input  logic        i_connect,
    input  logic        i_enabled,
    input  logic        i_reset_active,
    input  logic        i_suspended,
    input  logic [1:0]  i_speed,
    input  logic [1:0]  i_line,
    input  logic [10:0] i_frame,
    // PORT_CTRL levels toward the CDC
    output logic        o_run,
    output logic        o_power,
    output logic        o_reset_port,
    output logic        o_suspend,
    output logic        o_resume,
    // Transaction request fields toward the MAC (via the CDC's wires)
    output logic [1:0]  o_pid_sel,
    output logic [6:0]  o_devaddr,
    output logic [3:0]  o_endpoint,
    output logic        o_toggle,
    output logic [6:0]  o_length,
    // Line-capture instrument: commands out (toggles + levels), state
    // in (levels synchronized upstream; TRIG_ADDR stable under FROZEN)
    output logic        o_cap_arm_toggle,
    output logic        o_cap_disarm_toggle,
    output logic        o_cap_force,
    output logic [2:0]  o_cap_trig_sel,
    input  logic        i_cap_armed,
    input  logic        i_cap_frozen,
    input  logic [15:0] i_cap_trig_addr,
    output logic [15:0] o_cap_rd_addr,
    input  logic [15:0] i_cap_rd_data,
    // Transmitted-marker count (gray-crossed upstream)
    input  logic [15:0] i_sof_tx
);

    logic [6:0] reg_off;
    assign reg_off = {i_addr[6:2], 2'b00};

    logic is_data;
    assign is_data = i_addr[6];   // 0x40..0x7C, the DATA window

    // ── Register state ───────────────────────────────────────────────
    logic [2:0]  irq_status_q;
    logic [2:0]  irq_enable_q;
    logic [4:0]  port_ctrl_q;     // {RESUME, SUSPEND, RUN, RESET, POWER}
    logic [16:0] token_q;         // packed as the TOKEN register lays out
    logic [6:0]  length_q;

    // Line-capture control: the commanded arm state (readback), the
    // trigger selects, the command toggles toward the recorder, the
    // force level, and the readout index.
    logic        cap_arm_q;
    logic [2:0]  cap_trig_sel_q;
    logic        cap_arm_tgl_q, cap_disarm_tgl_q;
    logic        cap_force_q;
    logic [15:0] cap_addr_q;

    assign o_cap_arm_toggle    = cap_arm_tgl_q;
    assign o_cap_disarm_toggle = cap_disarm_tgl_q;
    assign o_cap_force         = cap_force_q;
    assign o_cap_trig_sel      = cap_trig_sel_q;
    assign o_cap_rd_addr       = cap_addr_q;

    assign {o_resume, o_suspend, o_run, o_reset_port, o_power} = port_ctrl_q;
    assign o_pid_sel  = token_q[1:0];
    assign o_devaddr  = token_q[10:4];
    assign o_endpoint = token_q[14:11];
    assign o_toggle   = token_q[16];
    assign o_length   = length_q;

    assign o_irq = |(irq_status_q & irq_enable_q);

    // ── DATA byte sequencer + the stalled-access state ───────────────
    //
    // IDLE accepts one bus access. A plain register answers on the next
    // cycle (S_REG). A DATA write streams the four lanes into the buffer
    // (S_WR); a DATA read pipelines four buffer reads (S_RD) — the byte
    // for the address presented in cycle n arrives in n+1 — then answers.
    // S_HOLD parks until the master releases the strobe, so a held
    // request is served exactly once.
    typedef enum logic [2:0] { S_IDLE, S_REG, S_WR, S_RD, S_HOLD } state_e;
    state_e state_q, state_d;

    logic [2:0] seq_q, seq_d;         // byte lane / pipeline step
    logic [31:0] rdata_q, rdata_d;

    logic access_start;
    assign access_start = (state_q == S_IDLE) && (i_re || i_we);

    assign o_busy = (i_re || i_we) && (state_q != S_HOLD) &&
                    !(i_we && state_q == S_IDLE && !is_data);

    // Plain-register writes complete in the accept cycle itself.
    logic reg_write;
    assign reg_write = access_start && i_we && !is_data;

    // PORT_STATUS.SPEED speaks the programmer contract (0 none, 1 low,
    // 2 full), not the seam's UTMI XcvrSelect encoding (1 full, 2 low)
    // the MAC carries — the hardware-internal coding must not leak into
    // the register interface, and "none" has no UTMI code at all.
    logic [1:0] speed_field;
    always_comb begin
        if (!i_connect)
            speed_field = 2'd0;
        else if (i_speed == 2'b10)   // usb_speed_e low-speed
            speed_field = 2'd1;
        else
            speed_field = 2'd2;
    end

    // The read image of every plain register.
    logic [31:0] reg_rdata;
    always_comb begin
        case (reg_off)
            USBHC_REG_CAP:         reg_rdata = {13'd0, 1'b1, i_caps[1],
                                                i_caps[0],
                                                8'(BUF_BYTES), 8'd1};
            USBHC_REG_CAP_CTRL:    reg_rdata = {28'd0, cap_trig_sel_q,
                                                cap_arm_q};
            USBHC_REG_CAP_STATUS:  reg_rdata = {i_cap_trig_addr,
                                                8'(CAP_DEPTH_LOG2), 6'd0,
                                                i_cap_frozen, i_cap_armed};
            USBHC_REG_CAP_ADDR:    reg_rdata = {16'd0, cap_addr_q};
            USBHC_REG_CAP_DATA:    reg_rdata = {16'd0, i_cap_rd_data};
            USBHC_REG_SOF_TX:      reg_rdata = {16'd0, i_sof_tx};
            USBHC_REG_IRQ_STATUS:  reg_rdata = {29'd0, irq_status_q};
            USBHC_REG_IRQ_ENABLE:  reg_rdata = {29'd0, irq_enable_q};
            USBHC_REG_PORT_STATUS: reg_rdata = {22'd0, i_line, 2'b00,
                                                speed_field, i_suspended,
                                                i_reset_active, i_enabled,
                                                i_connect};
            USBHC_REG_PORT_CTRL:   reg_rdata = {27'd0, port_ctrl_q};
            USBHC_REG_FRAME:       reg_rdata = {21'd0, i_frame};
            USBHC_REG_TOKEN:       reg_rdata = {15'd0, token_q};
            USBHC_REG_XFER_STATUS: reg_rdata = {17'd0, i_rxlen, 3'd0,
                                                i_rxtoggle, i_result,
                                                irq_status_q[USBHC_IRQ_XFER_DONE]};
            default:               reg_rdata = 32'd0;
        endcase
    end

    always_comb begin
        state_d      = state_q;
        seq_d        = seq_q;
        rdata_d      = rdata_q;
        o_cbuf_addr  = {1'b0, i_addr[5:2], seq_q[1:0]};
        o_cbuf_wdata = 8'd0;
        o_cbuf_we    = 1'b0;
        o_start      = 1'b0;

        case (state_q)
            S_IDLE: begin
                seq_d = 3'd0;
                if (i_re) begin
                    if (is_data) begin
                        state_d     = S_RD;
                        o_cbuf_addr = {1'b0, i_addr[5:2], 2'b00};
                        seq_d       = 3'd1;
                    end else begin
                        rdata_d = reg_rdata;
                        state_d = S_REG;
                    end
                end else if (i_we) begin
                    if (is_data) begin
                        o_cbuf_addr  = {1'b0, i_addr[5:2], 2'b00};
                        o_cbuf_we    = 1'b1;
                        o_cbuf_wdata = i_wdata[7:0];
                        seq_d        = 3'd1;
                        state_d      = S_WR;
                    end else begin
                        // Plain-register write: applied below, no stall.
                        if (reg_off == USBHC_REG_XFER_CTRL && i_wdata[16])
                            o_start = 1'b1;
                        state_d = S_HOLD;
                    end
                end
            end
            S_REG: state_d = S_HOLD;
            S_WR: begin
                o_cbuf_we    = 1'b1;
                o_cbuf_wdata = (seq_q[1:0] == 2'd1) ? i_wdata[15:8]
                             : (seq_q[1:0] == 2'd2) ? i_wdata[23:16]
                                                    : i_wdata[31:24];
                seq_d = seq_q + 3'd1;
                if (seq_q == 3'd3)
                    state_d = S_HOLD;
            end
            S_RD: begin
                // The byte for the address presented in cycle n arrives
                // in n+1: lane k lands while address k+1 is presented.
                seq_d = seq_q + 3'd1;
                case (seq_q)
                    3'd1: rdata_d[7:0]   = i_cbuf_rdata;
                    3'd2: rdata_d[15:8]  = i_cbuf_rdata;
                    3'd3: rdata_d[23:16] = i_cbuf_rdata;
                    default: begin
                        rdata_d[31:24] = i_cbuf_rdata;
                        state_d = S_HOLD;
                    end
                endcase
            end
            S_HOLD: begin
                if (!i_re && !i_we)
                    state_d = S_IDLE;
            end
            default: state_d = S_IDLE;
        endcase
    end

    assign o_rdata = rdata_q;

    // ── Register writes, interrupt sources, W1C ──────────────────────
    //
    // The sticky sources compose as set-over-clear: a W1C write races an
    // incoming event without losing it — a bit both set and cleared in
    // one cycle stays set, because the event is newer than the read that
    // the W1C answered.
    logic [2:0] irq_set, irq_clr;
    assign irq_set = {i_sof, i_port_change, i_done};
    assign irq_clr = (reg_write && reg_off == USBHC_REG_IRQ_STATUS)
                   ? i_wdata[2:0] : 3'd0;

    // CAP_DATA reads walk the buffer: the accept samples the data for
    // the current index, then the index advances for the next read.
    logic cap_data_read;
    assign cap_data_read = access_start && i_re && !is_data &&
                           (reg_off == USBHC_REG_CAP_DATA);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            irq_status_q     <= 3'd0;
            irq_enable_q     <= 3'd0;
            port_ctrl_q      <= 5'd0;
            token_q          <= 17'd0;
            length_q         <= 7'd0;
            cap_arm_q        <= 1'b0;
            cap_trig_sel_q   <= 3'd0;
            cap_arm_tgl_q    <= 1'b0;
            cap_disarm_tgl_q <= 1'b0;
            cap_force_q      <= 1'b0;
            cap_addr_q       <= 16'd0;
        end else begin
            irq_status_q <= (irq_status_q & ~irq_clr) | irq_set;

            // A stale force must not re-trigger the next capture; the
            // completed capture it forced clears it.
            if (i_cap_frozen)
                cap_force_q <= 1'b0;

            if (cap_data_read)
                cap_addr_q <= cap_addr_q + 16'd1;

            if (reg_write) begin
                case (reg_off)
                    USBHC_REG_IRQ_ENABLE:
                        irq_enable_q <= i_wdata[2:0];
                    USBHC_REG_PORT_CTRL:
                        port_ctrl_q <= i_wdata[4:0];
                    USBHC_REG_TOKEN:
                        token_q <= i_wdata[16:0];
                    USBHC_REG_XFER_CTRL:
                        length_q <= i_wdata[6:0];
                    USBHC_REG_CAP_CTRL: begin
                        cap_arm_q      <= i_wdata[0];
                        cap_trig_sel_q <= i_wdata[3:1];
                        cap_force_q    <= i_wdata[8];
                        if (i_wdata[0])
                            cap_arm_tgl_q <= ~cap_arm_tgl_q;
                        else
                            cap_disarm_tgl_q <= ~cap_disarm_tgl_q;
                    end
                    USBHC_REG_CAP_ADDR:
                        cap_addr_q <= i_wdata[15:0];
                    default: ;
                endcase
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q <= S_IDLE;
            seq_q   <= 3'd0;
            rdata_q <= 32'd0;
        end else begin
            state_q <= state_d;
            seq_q   <= seq_d;
            rdata_q <= rdata_d;
        end
    end
endmodule
