// SDRAM clock-domain-crossing bridge — depth-2 async FIFO.
//
// Sits between `sdram_bus_adapter` (system clock) and `sdram_ctrl`
// (SDRAM clock).  Up to 2 outstanding requests at any time; preserves
// request order so responses are delivered in the same order they
// were issued.  This decouples the SD-side controller's per-word
// processing time from the sys-side request-acceptance latency:
// sys can push request N+1 while the SD side is still working on N,
// and the SD side can immediately start N+1 the moment N retires
// (no req-tog round trip).
//
// Pointer scheme (standard async FIFO):
//   • 2-bit binary read/write pointers (mod 4).  Slot index = ptr[0].
//   • Pointers cross between domains as Gray code; 2-bit Gray decode
//     is bin[1]=g[1], bin[0]=g[1]^g[0].
//   • Empty: rptr_bin == wptr_bin_synced (SD side check).
//   • Full:  (wptr_bin - rptr_bin_synced) == 2 (sys side check).
//   • Retire: sys's local rptr lags behind the synchronized SD rptr,
//     each step of lag = one slot to deliver.
//
// CDC discipline (data-before-valid):
//   • Per-slot wide payloads (we/addr/wdata/byte_en) are written by
//     the sys side BEFORE bumping wptr_bin.  The SD side reads them
//     only after the synchronized wptr_gray has propagated (≥ 2 SD
//     cycles), so the data has been stable for many cycles.
//   • Per-slot rsp_data is written by SD BEFORE bumping rptr_bin and
//     read by sys only after the synchronized rptr_gray has
//     propagated.  Same discipline, opposite direction.
//   • The 1-bit Gray-pointer bits cross via 2-FF synchronizers; an
//     extra delay register on the sys side enables edge detection
//     (used to step the local rptr forward).
//
// Request acceptance is a single-cycle valid/ready transfer:
// `o_sys_req_ready` is combinational (`!sys_full`), so on any cycle the
// master drives a valid request with room available, the slot latch and
// the ready acknowledgment happen on the SAME edge.  The master must
// therefore observe ready and drop/advance its request the next cycle
// (standard handshake).  Latching and acknowledging atomically is
// load-bearing: the speculative master can change what it drives every
// cycle (a spec push one cycle, a real push the next on a mispredict),
// so a registered/delayed ready would let the master attribute an
// acknowledgment to a different request than the one actually latched.
//
//   sys side                                        sdram side
//   ────────                                        ──────────
//   adapter ─req──► [if !full: latch slot[wptr[0]],   [sync wptr_gray;
//                    bump wptr_bin, ready=!full]       when not empty,
//                                                      drive ctrl req
//                                                      from slot[rptr[0]]]
//                                          │
//                                          ▼
//   adapter ◄─done─[sync rptr_gray; on advance        [latch rsp_data
//                   pulse done/rsp_valid for each      into rsp[rptr[0]];
//                   retired slot in order;             bump rptr_bin]
//                   bump local rptr_bin]
//
// Latency added by the bridge (on top of controller timings):
//   • Request first-edge: ~3 sdram cycles (sync + advance)
//   • Done first-edge:    ~3 sys cycles
// Same per-transaction figures as the previous depth-1 design;
// deepening doesn't add latency, it removes the serial dependency
// between successive transactions when the master pipelines.

// keep_hierarchy: paired with the attribute on sdram_ctrl. Prevents
// ABC from sharing LUT4s across the controller/CDC boundary. See
// the rationale comment in sdram_ctrl.sv.
(* keep_hierarchy = "yes" *)
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
    output logic        o_sys_req_ready,    // combinational: high when a slot is free

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
    logic [1:0] sys_wptr_bin;
    logic [1:0] sys_rptr_bin;       // local: how many slots have retired
    logic [1:0] sd_rptr_gray_s1, sd_rptr_gray_s2;

    // Per-slot payload — written by sys, read by SD
    logic        slot_we      [0:1];
    logic [31:0] slot_addr    [0:1];
    logic [31:0] slot_wdata   [0:1];
    logic [3:0]  slot_byte_en [0:1];

    // ─────────────────────────────────────────────────────────
    // SD-domain state
    // ─────────────────────────────────────────────────────────
    logic [1:0] sd_rptr_bin;
    logic [1:0] sys_wptr_gray_s1, sys_wptr_gray_s2;

    typedef enum logic [1:0] {
        SD_IDLE,
        SD_DRIVE_REQ,
        SD_WAIT_DONE
    } sd_state_t;
    sd_state_t sd_state;

    // Per-slot response data — written by SD, read by sys
    logic [31:0] rsp_data [0:1];

    // ─────────────────────────────────────────────────────────
    // Gray encode/decode (combinational)
    // ─────────────────────────────────────────────────────────
    wire [1:0] sys_wptr_gray = sys_wptr_bin ^ (sys_wptr_bin >> 1);
    wire [1:0] sd_rptr_gray  = sd_rptr_bin  ^ (sd_rptr_bin  >> 1);

    // 2-bit Gray decode: bin[1]=g[1]; bin[0]=g[1]^g[0]
    wire [1:0] sd_rptr_bin_synced = {
        sd_rptr_gray_s2[1],
        sd_rptr_gray_s2[1] ^ sd_rptr_gray_s2[0]
    };
    wire [1:0] sys_wptr_bin_synced = {
        sys_wptr_gray_s2[1],
        sys_wptr_gray_s2[1] ^ sys_wptr_gray_s2[0]
    };

    // Outstanding requests (mod 4): accepted by the sys side but not yet
    // retired to the master (response delivered).  Full at depth 2.
    //
    // This bounds the whole request->response pipeline using the
    // sys-local retire pointer, NOT the SD read pointer.  A slot frees
    // for SD reuse the moment the SD side reads it (sd_rptr), but the
    // master's matching depth-2 response/tag tracking only drains when
    // the response is delivered (sys_rptr, which lags sd_rptr).  Gating
    // accept on sd_rptr would let the master issue a third request into
    // its depth-2 tracking during that lag.  Counting against sys_rptr
    // keeps accept in one clock domain and exactly aligned with response
    // delivery, so the bridge and master never disagree on occupancy.
    wire [1:0] outstanding_sys = sys_wptr_bin - sys_rptr_bin;
    wire       sys_full        = (outstanding_sys == 2'd2);
    wire       sd_empty        = (sd_rptr_bin == sys_wptr_bin_synced);

    // Combinational accept: a free slot can take a request this cycle.
    // Ready and the slot latch share this condition on the same edge, so
    // the master's accepted-this-cycle view always matches what is
    // latched (see the request-acceptance note in the header).
    assign o_sys_req_ready = !sys_full;

    // Sys-side retirement detector: local rptr lags synced SD rptr.
    wire sys_has_retired = (sys_rptr_bin != sd_rptr_bin_synced);

    // ─────────────────────────────────────────────────────────
    // Sys side: synchronize SD rptr_gray; handle launches and retires
    // ─────────────────────────────────────────────────────────
    always_ff @(posedge i_sys_clk) begin
        if (i_sys_rst) begin
            sys_wptr_bin     <= '0;
            sys_rptr_bin     <= '0;
            sd_rptr_gray_s1  <= '0;
            sd_rptr_gray_s2  <= '0;
            o_sys_done       <= 1'b0;
            o_sys_rsp_valid  <= 1'b0;
            o_sys_rsp_data   <= '0;
        end else begin
            // 2-FF sync of SD rptr_gray
            sd_rptr_gray_s1 <= sd_rptr_gray;
            sd_rptr_gray_s2 <= sd_rptr_gray_s1;

            // Defaults: clear one-cycle pulses
            o_sys_done      <= 1'b0;
            o_sys_rsp_valid <= 1'b0;

            // Retire one slot per cycle while SD's rptr is ahead.
            // Wide rsp_data is safe to read because SD wrote it
            // before bumping its rptr (data-before-valid; sync
            // delay > 2 SD cycles ensures stability).
            //
            // rsp_valid pulses only for reads — writes report
            // completion via o_sys_done alone.  Without this gate,
            // a write retiring while a later read is also in flight
            // would spuriously latch garbage rdata at the master.
            if (sys_has_retired) begin
                o_sys_done      <= 1'b1;
                o_sys_rsp_valid <= !slot_we[sys_rptr_bin[0]];
                o_sys_rsp_data  <= rsp_data[sys_rptr_bin[0]];
                sys_rptr_bin    <= sys_rptr_bin + 2'd1;
            end

            // Accept a new request whenever a slot is free and the
            // master is presenting one.  This is the same condition as
            // the combinational o_sys_req_ready, so the latch and the
            // acknowledgment are one atomic event — the master's
            // accepted-this-cycle bookkeeping always names the request
            // actually stored here.  The master drops/advances its
            // request the next cycle, so a free slot is not re-latched.
            if (o_sys_req_ready && i_sys_req_valid) begin
                slot_we     [sys_wptr_bin[0]] <= i_sys_req_we;
                slot_addr   [sys_wptr_bin[0]] <= i_sys_req_addr;
                slot_wdata  [sys_wptr_bin[0]] <= i_sys_req_wdata;
                slot_byte_en[sys_wptr_bin[0]] <= i_sys_req_byte_en;
                sys_wptr_bin    <= sys_wptr_bin + 2'd1;
            end
        end
    end

    // ─────────────────────────────────────────────────────────
    // SD side: synchronize sys wptr_gray; drive controller
    // ─────────────────────────────────────────────────────────
    assign o_sd_rsp_ready = 1'b1;

    always_ff @(posedge i_sd_clk) begin
        if (i_sd_rst) begin
            sd_rptr_bin       <= '0;
            sys_wptr_gray_s1  <= '0;
            sys_wptr_gray_s2  <= '0;
            sd_state          <= SD_IDLE;
            o_sd_req_valid    <= 1'b0;
            o_sd_req_we       <= 1'b0;
            o_sd_req_addr     <= '0;
            o_sd_req_wdata    <= '0;
            o_sd_req_byte_en  <= '0;
        end else begin
            // 2-FF sync of sys wptr_gray
            sys_wptr_gray_s1 <= sys_wptr_gray;
            sys_wptr_gray_s2 <= sys_wptr_gray_s1;

            case (sd_state)
                SD_IDLE: begin
                    if (!sd_empty) begin
                        // Sample wide payload from slot[rptr[0]].
                        // Safe because sys wrote it before bumping
                        // its wptr_bin (data-before-valid).
                        o_sd_req_we      <= slot_we     [sd_rptr_bin[0]];
                        o_sd_req_addr    <= slot_addr   [sd_rptr_bin[0]];
                        o_sd_req_wdata   <= slot_wdata  [sd_rptr_bin[0]];
                        o_sd_req_byte_en <= slot_byte_en[sd_rptr_bin[0]];
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
                        // Latch response (reads) and retire the slot.
                        // Bumping rptr_bin makes this slot's rsp_data
                        // visible to sys after the gray pointer
                        // crosses.
                        if (i_sd_rsp_valid)
                            rsp_data[sd_rptr_bin[0]] <= i_sd_rsp_data;
                        sd_rptr_bin <= sd_rptr_bin + 2'd1;
                        sd_state    <= SD_IDLE;
                    end
                end

                default: sd_state <= SD_IDLE;
            endcase
        end
    end

    // ── Invariant (sim/synth-stripped) ───────────────────────────
    // The sys-side outstanding count never exceeds the depth-2 FIFO.
    // A value of 3 would mean a slot was latched while full — the
    // failure mode if the master ever re-presents an already-accepted
    // request without the combinational ready dropping, or if the
    // wptr/rptr bookkeeping desyncs after a refactor.
    assert property (@(posedge i_sys_clk) disable iff (i_sys_rst)
        (outstanding_sys <= 2'd2))
        else $error("sdram_cdc: outstanding_sys > 2 — slot FIFO overflow");

endmodule
