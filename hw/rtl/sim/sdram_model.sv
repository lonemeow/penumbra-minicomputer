// Behavioral SDR SDRAM chip model for simulation.
//
// Models a single SDRAM chip on a shared 16-bit DQ bus.  Implements
// the JEDEC command set used by sdram_ctrl: ACTIVATE, READ, WRITE,
// PRECHARGE, AUTO_REFRESH, MODE_REGISTER_SET.  Burst length and CAS
// latency are derived from the mode register written during init.
//
// Storage: associative array keyed by (bank,row,col).  Verilator does
// not preallocate the full chip — pages are created as written.  This
// makes a 32 MB chip cheap to simulate.
//
// DQ output: combinational from registered state, with the CL latency
// implemented as a countdown.  Combinational drive (rather than a
// registered-output flop) avoids one extra cycle of model latency,
// matching the controller's expectation of CL+1 round-trip cycles
// (the +1 is the FPGA-side IOB sample).
//
// Protocol violations are flagged via $display + a counter rather than
// $fatal, so a single failure does not mask later issues.

module sdram_model
    import sdram_pkg::*;
#(
    parameter int ROW_BITS = 13,
    parameter int COL_BITS = 9,
    parameter int BA_BITS  = 2,
    parameter int DQ_BITS  = 16
)(
    input  logic                  i_clk,
    input  logic                  i_cke,
    input  logic                  i_csn,
    input  logic                  i_rasn,
    input  logic                  i_casn,
    input  logic                  i_wen,
    input  logic [ROW_BITS-1:0]   i_a,
    input  logic [BA_BITS-1:0]    i_ba,
    input  logic [DQ_BITS/8-1:0]  i_dqm,
    inout  wire  [DQ_BITS-1:0]    io_d
);

    localparam int N_BANKS = 1 << BA_BITS;

    // ── Storage: sparse associative array ──────────────────
    logic [DQ_BITS-1:0] mem [int unsigned];

    function automatic int unsigned mem_idx(input int unsigned bank,
                                            input int unsigned row,
                                            input int unsigned col);
        return (bank << (ROW_BITS + COL_BITS)) | (row << COL_BITS) | col;
    endfunction

    // ── Per-bank state ─────────────────────────────────────
    typedef enum logic [0:0] { B_IDLE, B_ACTIVE } bank_state_t;
    bank_state_t         bank_state [0:N_BANKS-1];
    logic [ROW_BITS-1:0] bank_row   [0:N_BANKS-1];

    // ── Mode register (latched on MRS) ─────────────────────
    logic [2:0] mr_bl;
    logic [2:0] mr_cl;

    // ── Decoded command from chip pins ─────────────────────
    wire [3:0] cmd_in = {i_csn, i_rasn, i_casn, i_wen};

    // ── Read pipeline state ────────────────────────────────
    logic                  rd_active;
    logic [BA_BITS-1:0]    rd_bank;
    logic [COL_BITS-1:0]   rd_col;
    logic [3:0]            rd_beat;
    logic [3:0]            rd_lat;
    logic                  rd_auto_pre;

    // ── Write tracking (BL=2 today; extend if BL changes) ──
    logic                  wr_active;
    logic [BA_BITS-1:0]    wr_bank;
    logic [COL_BITS-1:0]   wr_col;
    logic                  wr_auto_pre;

    // ── Column offsets within in-flight bursts ─────────────
    wire [COL_BITS-1:0] rd_col_eff = rd_col + COL_BITS'(rd_beat);
    wire [COL_BITS-1:0] wr_col_p1  = wr_col + COL_BITS'(1);

    // ── DQ output (combinational from registered state) ────
    logic [DQ_BITS-1:0]    dq_drive;
    logic                  dq_drive_en;

    assign io_d = dq_drive_en ? dq_drive : {DQ_BITS{1'bz}};

    always_comb begin
        dq_drive_en = 1'b0;
        dq_drive    = '0;
        if (rd_active && rd_lat == 0) begin
            dq_drive_en = 1'b1;
            dq_drive    = mem[mem_idx(int'(rd_bank),
                                      int'(bank_row[rd_bank]),
                                      int'(rd_col_eff))];
        end
    end

    // ── Diagnostics ─────────────────────────────────────────
    integer protocol_errors;
    initial protocol_errors = 0;

    // ── Initialization ──────────────────────────────────────
    initial begin
        for (int b = 0; b < N_BANKS; b++) begin
            bank_state[b] = B_IDLE;
            bank_row[b]   = '0;
        end
        rd_active   = 0;
        wr_active   = 0;
        mr_bl       = SDRAM_BL_2;
        mr_cl       = 3'd2;
        rd_bank     = '0;
        rd_col      = '0;
        rd_beat     = '0;
        rd_lat      = '0;
        rd_auto_pre = 0;
        wr_bank     = '0;
        wr_col      = '0;
        wr_auto_pre = 0;
    end

    // ── Helpers ─────────────────────────────────────────────
    function automatic int unsigned bl_to_int(input logic [2:0] bl);
        case (bl)
            SDRAM_BL_1: return 1;
            SDRAM_BL_2: return 2;
            SDRAM_BL_4: return 4;
            SDRAM_BL_8: return 8;
            default:    return 1;
        endcase
    endfunction

    // ── Main per-cycle handler ─────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_cke) begin
            // CKE low — clock suspended; no state changes.
        end else begin
            // ── Advance in-flight read ───────────────────
            if (rd_active) begin
                if (rd_lat != 0) begin
                    rd_lat <= rd_lat - 4'd1;
                end else begin
                    rd_beat <= rd_beat + 4'd1;
                    if (int'(rd_beat) == int'(bl_to_int(mr_bl)) - 1) begin
                        rd_active <= 1'b0;
                        if (rd_auto_pre)
                            bank_state[rd_bank] <= B_IDLE;
                    end
                end
            end

            // ── Advance in-flight write (BL=2: one extra beat) ──
            if (wr_active) begin
                if (!i_dqm[0])
                    mem[mem_idx(int'(wr_bank),
                                int'(bank_row[wr_bank]),
                                int'(wr_col_p1))][7:0]  <= io_d[7:0];
                if (!i_dqm[1])
                    mem[mem_idx(int'(wr_bank),
                                int'(bank_row[wr_bank]),
                                int'(wr_col_p1))][15:8] <= io_d[15:8];
                wr_active <= 1'b0;
                if (wr_auto_pre)
                    bank_state[wr_bank] <= B_IDLE;
            end

            // ── Decode current command ───────────────────
            case (cmd_in)
                SDRAM_CMD_NOP, SDRAM_CMD_INHIBIT: begin
                    // no-op
                end

                SDRAM_CMD_ACTIVATE: begin
                    if (bank_state[i_ba] != B_IDLE) begin
                        $display("[sdram_model %0t] PROTOCOL: ACTIVATE on non-IDLE bank %0d", $time, i_ba);
                        protocol_errors <= protocol_errors + 1;
                    end
                    bank_state[i_ba] <= B_ACTIVE;
                    bank_row[i_ba]   <= i_a;
                end

                SDRAM_CMD_READ: begin
                    if (bank_state[i_ba] != B_ACTIVE) begin
                        $display("[sdram_model %0t] PROTOCOL: READ on non-ACTIVE bank %0d", $time, i_ba);
                        protocol_errors <= protocol_errors + 1;
                    end
                    if (rd_active || wr_active) begin
                        $display("[sdram_model %0t] PROTOCOL: READ overlaps in-flight access", $time);
                        protocol_errors <= protocol_errors + 1;
                    end
                    rd_active   <= 1'b1;
                    rd_bank     <= i_ba;
                    rd_col      <= i_a[COL_BITS-1:0];
                    rd_beat     <= '0;
                    rd_lat      <= 4'(mr_cl) - 4'd1;
                    rd_auto_pre <= i_a[10];
                end

                SDRAM_CMD_WRITE: begin
                    if (bank_state[i_ba] != B_ACTIVE) begin
                        $display("[sdram_model %0t] PROTOCOL: WRITE on non-ACTIVE bank %0d", $time, i_ba);
                        protocol_errors <= protocol_errors + 1;
                    end
                    if (rd_active || wr_active) begin
                        $display("[sdram_model %0t] PROTOCOL: WRITE overlaps in-flight access", $time);
                        protocol_errors <= protocol_errors + 1;
                    end
                    // Sample beat 0 immediately (data already on DQ from controller)
                    if (!i_dqm[0])
                        mem[mem_idx(int'(i_ba),
                                    int'(bank_row[i_ba]),
                                    int'(i_a[COL_BITS-1:0]))][7:0]  <= io_d[7:0];
                    if (!i_dqm[1])
                        mem[mem_idx(int'(i_ba),
                                    int'(bank_row[i_ba]),
                                    int'(i_a[COL_BITS-1:0]))][15:8] <= io_d[15:8];

                    if (mr_bl == SDRAM_BL_2) begin
                        wr_active   <= 1'b1;
                        wr_bank     <= i_ba;
                        wr_col      <= i_a[COL_BITS-1:0];
                        wr_auto_pre <= i_a[10];
                    end else if (i_a[10]) begin
                        bank_state[i_ba] <= B_IDLE;
                    end
                end

                SDRAM_CMD_PRECHARGE: begin
                    if (i_a[10]) begin
                        for (int b = 0; b < N_BANKS; b++)
                            bank_state[b] <= B_IDLE;
                    end else begin
                        bank_state[i_ba] <= B_IDLE;
                    end
                end

                SDRAM_CMD_REFRESH: begin
                    for (int b = 0; b < N_BANKS; b++) begin
                        if (bank_state[b] != B_IDLE) begin
                            $display("[sdram_model %0t] PROTOCOL: REFRESH while bank %0d not IDLE", $time, b);
                            protocol_errors <= protocol_errors + 1;
                        end
                    end
                end

                SDRAM_CMD_MODE_SET: begin
                    for (int b = 0; b < N_BANKS; b++) begin
                        if (bank_state[b] != B_IDLE) begin
                            $display("[sdram_model %0t] PROTOCOL: MRS while bank %0d not IDLE", $time, b);
                            protocol_errors <= protocol_errors + 1;
                        end
                    end
                    mr_bl  <= i_a[2:0];
                    mr_cl  <= i_a[6:4];
                end

                default: begin
                    $display("[sdram_model %0t] PROTOCOL: unknown command %b", $time, cmd_in);
                    protocol_errors <= protocol_errors + 1;
                end
            endcase
        end
    end

endmodule
