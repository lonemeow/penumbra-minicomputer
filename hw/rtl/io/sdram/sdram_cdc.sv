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
//   sys side                                        sdram side
//   ────────                                        ──────────
//   adapter ─req──► [if !full: latch slot[wptr[0]],   [sync wptr_gray;
//                    bump wptr_bin, pulse req_ready]   when not empty,
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
    logic [1:0] sys_wptr_bin;
    logic [1:0] sys_rptr_bin;       // local: how many slots have retired
    logic [1:0] sd_rptr_gray_s1, sd_rptr_gray_s2;

    // 1-cycle accept-deadzone.  The master combinationally drives
    // i_sys_req_valid for the cycle in which it observes the registered
    // o_sys_req_ready pulse; without this gate, the bridge would
    // re-accept the same request the next cycle (depth-2 makes
    // !sys_full remain true after one accept).  The deadzone forces
    // the master to drop and re-assert valid between requests, same
    // discipline as the depth-1 sys_busy gate but per-cycle instead
    // of per-transaction.
    logic       just_accepted;

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

    // Outstanding count (mod 4); full at depth 2.
    wire [1:0] outstanding_sys = sys_wptr_bin - sd_rptr_bin_synced;
    wire       sys_full        = (outstanding_sys == 2'd2);
    wire       sd_empty        = (sd_rptr_bin == sys_wptr_bin_synced);

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
            just_accepted    <= 1'b0;
            o_sys_req_ready  <= 1'b0;
            o_sys_done       <= 1'b0;
            o_sys_rsp_valid  <= 1'b0;
            o_sys_rsp_data   <= '0;
        end else begin
            // 2-FF sync of SD rptr_gray
            sd_rptr_gray_s1 <= sd_rptr_gray;
            sd_rptr_gray_s2 <= sd_rptr_gray_s1;

            // Defaults: clear one-cycle pulses and the deadzone
            o_sys_req_ready <= 1'b0;
            o_sys_done      <= 1'b0;
            o_sys_rsp_valid <= 1'b0;
            just_accepted   <= 1'b0;

            // Retire one slot per cycle while SD's rptr is ahead.
            // Wide rsp_data is safe to read because SD wrote it
            // before bumping its rptr (data-before-valid; sync
            // delay > 2 SD cycles ensures stability).
            if (sys_has_retired) begin
                o_sys_done      <= 1'b1;
                o_sys_rsp_valid <= 1'b1;
                o_sys_rsp_data  <= rsp_data[sys_rptr_bin[0]];
                sys_rptr_bin    <= sys_rptr_bin + 2'd1;
            end

            // Accept a new request if there's room and we didn't
            // accept one last cycle (forces master to drop valid
            // before next request — see comment on `just_accepted`).
            if (!sys_full && i_sys_req_valid && !just_accepted) begin
                slot_we     [sys_wptr_bin[0]] <= i_sys_req_we;
                slot_addr   [sys_wptr_bin[0]] <= i_sys_req_addr;
                slot_wdata  [sys_wptr_bin[0]] <= i_sys_req_wdata;
                slot_byte_en[sys_wptr_bin[0]] <= i_sys_req_byte_en;
                sys_wptr_bin    <= sys_wptr_bin + 2'd1;
                o_sys_req_ready <= 1'b1;
                just_accepted   <= 1'b1;
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

endmodule
