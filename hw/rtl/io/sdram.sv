// Penumbra SDRAM Controller — simple polled controller for FPGA
//
// Targets all SDRAM variants found on ULX3S boards (256 Mbit / 32 MB,
// 16-bit data, 4 banks). Default timing parameters are safe for the
// slowest variant (ISSI IS42S16160G-7TL, tRCD/tRP=21ns) at 12.5 MHz.
//
// Architecture:
//   - Close-after-access (auto-precharge via A10=1)
//   - Burst length 2 (reads/writes both 16-bit halves per CPU word)
//   - CAS latency 2 (safe for all variants at ≤133 MHz)
//   - Periodic auto-refresh with priority over CPU requests
//
// The 32-bit CPU bus maps to the 16-bit SDRAM via two-beat bursts:
//   Beat 0: low  half-word (CPU data[15:0],  byte_en[1:0] → DQM)
//   Beat 1: high half-word (CPU data[31:16], byte_en[3:2] → DQM)
//
// Bus protocol: assert o_busy from request until data is valid (reads)
// or write is committed (writes). Same contract as fpga_ram/simple_mem.

module sdram
    import penumbra_pkg::*;
#(
    // ── SDRAM geometry ─────────────────────────────────────
    parameter ROW_BITS  = 13,       // Row address width (8192 rows)
    parameter COL_BITS  = 9,        // Column address width (512 or 1024)
    parameter BA_BITS   = 2,        // Bank address width (4 banks)
    parameter DQ_BITS   = 16,       // Data bus width

    // ── Timing in clock cycles ─────────────────────────────
    // Defaults safe for all ULX3S SDRAM variants at 12.5 MHz.
    // To recalculate for clock freq F:
    //   T_xxx = ceil(t_xxx_ns / (1e9 / F))
    // e.g. at 100 MHz: T_RCD = ceil(21/10) = 3
    parameter T_POWERUP = 2500,     // 200 µs power-up delay (2500 @ 12.5 MHz)
    parameter T_RCD     = 1,        // ACTIVATE → READ/WRITE (min 1)
    parameter T_RP      = 1,        // PRECHARGE → next command (min 1)
    parameter T_RFC     = 1,        // AUTO REFRESH cycle time (min 1)
    parameter T_WR      = 2,        // Write recovery (always ≥2 CLK)
    parameter T_MRD     = 2,        // Mode register set delay (always ≥2 CLK)
    parameter CAS_LATENCY = 2,      // CL=2 for ≤133 MHz, CL=3 for >133 MHz
    parameter REFRESH_INTERVAL = 97 // 64 ms / 8192 rows, in clock cycles
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU bus interface (same as fpga_ram / simple_mem) ───
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Physical SDRAM pins ────────────────────────────────
    output logic                    o_sdram_clk,
    output logic                    o_sdram_cke,
    output logic                    o_sdram_csn,
    output logic                    o_sdram_rasn,
    output logic                    o_sdram_casn,
    output logic                    o_sdram_wen,
    output logic [ROW_BITS-1:0]     o_sdram_a,
    output logic [BA_BITS-1:0]      o_sdram_ba,
    inout  wire  [DQ_BITS-1:0]      io_sdram_d,
    output logic [DQ_BITS/8-1:0]    o_sdram_dqm,

    // ── Debug ──────────────────────────────────────────────
    output logic        o_dbg_init_done,    // Init sequence completed
    output logic        o_dbg_access_done   // At least one access completed
);

    // ── SDRAM command encoding {CSn, RASn, CASn, WEn} ──────
    localparam CMD_NOP       = 4'b0111;
    localparam CMD_ACTIVATE  = 4'b0011;
    localparam CMD_READ      = 4'b0101;
    localparam CMD_WRITE     = 4'b0100;
    localparam CMD_PRECHARGE = 4'b0010;
    localparam CMD_REFRESH   = 4'b0001;
    localparam CMD_MODE_SET  = 4'b0000;
    localparam CMD_INHIBIT   = 4'b1111;

    // ── Mode register value ────────────────────────────────
    // A[2:0]=001 (BL=2), A[3]=0 (sequential), A[6:4]=CL, rest=0
    localparam [ROW_BITS-1:0] MODE_REG = {{(ROW_BITS-7){1'b0}}, CAS_LATENCY[2:0], 4'b0001};

    // ── State machine ──────────────────────────────────────
    typedef enum logic [3:0] {
        S_INIT_WAIT,        // Power-up delay
        S_INIT_PRECHARGE,   // PRECHARGE ALL
        S_INIT_REFRESH1,    // First AUTO REFRESH
        S_INIT_REFRESH2,    // Second AUTO REFRESH
        S_INIT_MODE,        // MODE REGISTER SET
        S_IDLE,             // Ready for requests
        S_REFRESH,          // Periodic AUTO REFRESH
        S_ACTIVATE,         // Row activation (wait T_RCD)
        S_READ,             // READ+AP issued, waiting CAS latency + burst
        S_WRITE,            // WRITE+AP issued, driving data burst
        S_RECOVERY          // Wait for auto-precharge (T_RP after burst)
    } state_t;

    state_t state;

    // ── Timing counter (shared across states) ──────────────
    logic [15:0] wait_cnt;

    // ── Refresh timer ──────────────────────────────────────
    logic [15:0] refresh_cnt;
    logic        refresh_needed;

    // ── Latched request ────────────────────────────────────
    logic [ROW_BITS-1:0] latch_row;
    logic [COL_BITS-1:0] latch_col;
    logic [BA_BITS-1:0]  latch_bank;
    logic [31:0]         latch_wdata;
    logic [3:0]          latch_byte_en;
    logic                latch_is_write;

    // ── Address mapping ────────────────────────────────────
    // CPU addr[0]                  → byte within 16-bit word (DQM)
    // CPU addr[COL_BITS:1]         → SDRAM column
    // CPU addr[COL_BITS+ROW_BITS:COL_BITS+1] → SDRAM row
    // CPU addr[COL_BITS+ROW_BITS+BA_BITS:COL_BITS+ROW_BITS+1] → bank
    wire [COL_BITS-1:0] addr_col  = i_addr[COL_BITS:1];
    wire [ROW_BITS-1:0] addr_row  = i_addr[COL_BITS+ROW_BITS:COL_BITS+1];
    wire [BA_BITS-1:0]  addr_bank = i_addr[COL_BITS+ROW_BITS+BA_BITS:COL_BITS+ROW_BITS+1];

    // ── Bidirectional data bus ──────────────────────────────
    logic [DQ_BITS-1:0] dq_out;
    logic                dq_oe;         // 1 = driving, 0 = high-Z
    logic [DQ_BITS-1:0] dq_in;

    assign io_sdram_d = dq_oe ? dq_out : {DQ_BITS{1'bz}};
    assign dq_in      = io_sdram_d;

    // ── Read data assembly ─────────────────────────────────
    logic [15:0] read_low;      // First burst beat (low half-word)
    logic [31:0] rdata_reg;     // Assembled 32-bit result

    // ── SDRAM clock output ─────────────────────────────────
    // At 12.5 MHz, forwarding system clock directly is fine.
    // For higher speeds, use ODDRX1F or PLL phase shift.
    assign o_sdram_clk = i_clk;
    assign o_sdram_cke = 1'b1;      // Always enabled

    // ── Command issue helper ───────────────────────────────
    logic [3:0] cmd;
    assign {o_sdram_csn, o_sdram_rasn, o_sdram_casn, o_sdram_wen} = cmd;

    // ── Burst beat counter (0 or 1 for BL=2) ──────────────
    logic beat;

    // ── Main state machine ─────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state        <= S_INIT_WAIT;
            cmd          <= CMD_INHIBIT;
            o_sdram_a    <= '0;
            o_sdram_ba   <= '0;
            o_sdram_dqm  <= '1;         // Mask all during init
            dq_oe        <= 1'b0;
            dq_out       <= '0;
            wait_cnt     <= T_POWERUP[15:0];
            refresh_cnt  <= '0;
            refresh_needed <= 1'b0;
            read_low     <= '0;
            rdata_reg    <= 32'hDEAD_BEEF;
            beat         <= 1'b0;
            latch_row    <= '0;
            latch_col    <= '0;
            latch_bank   <= '0;
            latch_wdata  <= '0;
            latch_byte_en <= '0;
            latch_is_write <= 1'b0;
        end else begin

            // ── Default: NOP, data bus high-Z, DQM masked ──
            cmd     <= CMD_NOP;
            dq_oe   <= 1'b0;
            o_sdram_dqm <= 2'b11;

            // ── Refresh timer (runs in all states) ─────────
            if (state != S_INIT_WAIT && state != S_INIT_PRECHARGE &&
                state != S_INIT_REFRESH1 && state != S_INIT_REFRESH2 &&
                state != S_INIT_MODE) begin
                if (refresh_cnt >= REFRESH_INTERVAL[15:0]) begin
                    refresh_cnt    <= '0;
                    refresh_needed <= 1'b1;
                end else begin
                    refresh_cnt <= refresh_cnt + 1;
                end
            end

            // ── State machine ──────────────────────────────
            case (state)

            // ════════════════════════════════════════════════
            // Initialization sequence
            // ════════════════════════════════════════════════

            S_INIT_WAIT: begin
                cmd <= CMD_INHIBIT;
                if (wait_cnt == 0)
                    state <= S_INIT_PRECHARGE;
                else
                    wait_cnt <= wait_cnt - 1;
            end

            S_INIT_PRECHARGE: begin
                cmd       <= CMD_PRECHARGE;
                o_sdram_a[10] <= 1'b1;      // A10=1 → precharge ALL banks
                wait_cnt  <= T_RP[15:0];
                state     <= S_INIT_REFRESH1;
            end

            S_INIT_REFRESH1: begin
                if (wait_cnt == 0) begin
                    cmd      <= CMD_REFRESH;
                    wait_cnt <= T_RFC[15:0];
                    state    <= S_INIT_REFRESH2;
                end else begin
                    wait_cnt <= wait_cnt - 1;
                end
            end

            S_INIT_REFRESH2: begin
                if (wait_cnt == 0) begin
                    cmd      <= CMD_REFRESH;
                    wait_cnt <= T_RFC[15:0];
                    state    <= S_INIT_MODE;
                end else begin
                    wait_cnt <= wait_cnt - 1;
                end
            end

            S_INIT_MODE: begin
                if (wait_cnt == 0) begin
                    cmd       <= CMD_MODE_SET;
                    o_sdram_a <= MODE_REG;
                    o_sdram_ba <= '0;
                    wait_cnt  <= T_MRD[15:0];
                    state     <= S_IDLE;
                end else begin
                    wait_cnt <= wait_cnt - 1;
                end
            end

            // ════════════════════════════════════════════════
            // Normal operation
            // ════════════════════════════════════════════════

            S_IDLE: begin
                if (wait_cnt != 0) begin
                    // Still waiting for T_MRD or previous T_RP
                    wait_cnt <= wait_cnt - 1;
                end else if (refresh_needed) begin
                    cmd            <= CMD_REFRESH;
                    refresh_needed <= 1'b0;
                    wait_cnt       <= T_RFC[15:0];
                    state          <= S_REFRESH;
                end else if (i_re || i_we) begin
                    // Latch request and start ACTIVATE
                    latch_row      <= addr_row;
                    latch_col      <= addr_col;
                    latch_bank     <= addr_bank;
                    latch_wdata    <= i_wdata;
                    latch_byte_en  <= i_byte_en;
                    latch_is_write <= i_we;

                    cmd        <= CMD_ACTIVATE;
                    o_sdram_a  <= addr_row;
                    o_sdram_ba <= addr_bank;
                    wait_cnt   <= (T_RCD > 1) ? T_RCD[15:0] - 1 : '0;
                    state      <= S_ACTIVATE;
                end
            end

            S_REFRESH: begin
                if (wait_cnt == 0)
                    state <= S_IDLE;
                else
                    wait_cnt <= wait_cnt - 1;
            end

            S_ACTIVATE: begin
                if (wait_cnt == 0) begin
                    // T_RCD satisfied — issue READ or WRITE with auto-precharge
                    o_sdram_ba <= latch_bank;
                    // Column address with A10=1 for auto-precharge.
                    // BL=2 burst starts at even column (bit 0 = 0).
                    o_sdram_a  <= '0;
                    o_sdram_a[COL_BITS-1:0] <= {latch_col[COL_BITS-1:1], 1'b0};
                    o_sdram_a[10] <= 1'b1;      // Auto-precharge

                    beat <= 1'b0;

                    if (latch_is_write) begin
                        cmd <= CMD_WRITE;
                        // SDRAM writes have 0-cycle data latency:
                        // first data beat must be on DQ at the WRITE command.
                        dq_oe       <= 1'b1;
                        dq_out      <= latch_wdata[15:0];
                        o_sdram_dqm <= ~latch_byte_en[1:0];
                        beat        <= 1'b1;   // Beat 0 done here, S_WRITE does beat 1
                        state <= S_WRITE;
                    end else begin
                        cmd <= CMD_READ;
                        o_sdram_dqm <= 2'b00;   // DQM has 2-clk read latency — start early
                        // +1 accounts for I/O pad round-trip: SDRAM drives data
                        // on sdram_clk rising edge, but FPGA samples on the SAME
                        // edge. I/O delay means data arrives a few ns late and is
                        // captured one FPGA cycle after the SDRAM outputs it.
                        wait_cnt <= CAS_LATENCY[15:0];
                        state <= S_READ;
                    end
                end else begin
                    wait_cnt <= wait_cnt - 1;
                end
            end

            // ── READ burst ─────────────────────────────────
            S_READ: begin
                o_sdram_dqm <= 2'b00;
                if (wait_cnt == 0) begin
                    beat <= ~beat;
                    if (!beat) begin
                        // Beat 0: low half-word
                        read_low <= dq_in;
                    end else begin
                        // Beat 1: high half-word
                        rdata_reg <= {dq_in, read_low};
                        // Wait tRP before next command
                        wait_cnt  <= T_RP[15:0] - 1;
                        state     <= S_RECOVERY;
                    end
                end else begin
                    wait_cnt <= wait_cnt - 1;
                end
            end

            // ── WRITE beat 1 (beat 0 driven in S_ACTIVATE) ──
            S_WRITE: begin
                dq_oe       <= 1'b1;
                dq_out      <= latch_wdata[31:16];
                o_sdram_dqm <= ~latch_byte_en[3:2];
                // tWR + tRP before next command
                wait_cnt    <= T_WR[15:0] + T_RP[15:0] - 1;
                state       <= S_RECOVERY;
            end

            S_RECOVERY: begin
                if (wait_cnt == 0)
                    state <= S_IDLE;
                else
                    wait_cnt <= wait_cnt - 1;
            end

            default: state <= S_INIT_WAIT;

            endcase
        end
    end

    // ── Debug indicators ─────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_dbg_init_done   <= 1'b0;
            o_dbg_access_done <= 1'b0;
        end else begin
            if (state == S_IDLE)
                o_dbg_init_done <= 1'b1;      // Latches on, never clears
            if (access_done)
                o_dbg_access_done <= 1'b1;    // Latches on first completion
        end
    end

    // ── Busy / data-valid signal ──────────────────────────
    // The cache needs exactly one cycle of !busy to latch read data
    // and advance to the next word.  We use a registered "done" pulse
    // that fires one cycle after the read or write completes.
    //
    // Read complete:  S_READ with wait_cnt=0 and beat=1 (high half latched)
    // Write complete: S_WRITE with beat=1 (second data beat committed)
    //
    // The done pulse fires during S_RECOVERY (one cycle after completion),
    // so rdata_reg already holds valid data from the previous cycle.
    logic access_done;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            access_done <= 1'b0;
        else
            access_done <= (state == S_READ && wait_cnt == 0 && beat) ||
                           (state == S_WRITE);
    end

    // Idle and ready: S_IDLE with no pending recovery, refresh, or request
    wire idle_ready = (state == S_IDLE) && (wait_cnt == 0) &&
                      !refresh_needed && !(i_re || i_we);

    // Busy: high unless done pulse (data valid) or genuinely idle
    assign o_busy  = !access_done && !idle_ready;
    assign o_rdata = rdata_reg;

endmodule
