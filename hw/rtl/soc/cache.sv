// Penumbra Cache — parameterized PIPT cache module
//
// Reusable for both I-cache and D-cache. Direct-mapped (NUM_WAYS=1)
// with write-through, write-no-allocate policy. Burst-fills entire
// cache line on read miss. Passes through when disabled (CTRL.enable=0)
// or uncacheable (i_cacheable=0), identical to cache_stub behavior.
//
// Parameters are exposed via the INFO sysreg so software can discover
// cache geometry at runtime (line size, sets, ways, type).
//
// Design principle: only the read-miss fill uses a state machine.
// Everything else is handled combinationally in S_IDLE:
//   - Uncacheable/disabled: pure pass-through (memory handles busy)
//   - Read hit: return cached data, busy=0
//   - Write hit: update cache + pass write to memory (memory handles busy)
//   - Write miss: pass write to memory (write-no-allocate)
//
// On read miss, the fill completes and returns to S_IDLE. The CPU
// (still in STALL with i_re held) re-evaluates and hits on the next
// cycle. This avoids forwarding mux complexity at the cost of one
// extra cycle per miss.

// verilator lint_off UNUSEDSIGNAL

module cache
    import penumbra_pkg::*;
#(
    parameter NUM_SETS   = 64,
    parameter LINE_WORDS = 4,
    parameter NUM_WAYS   = 1,
    parameter CACHE_TYPE = CACHE_TYPE_WT_WNA
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU side (post-MMU physical address) ───────────────
    input  logic [31:0] i_paddr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Memory / bus side ──────────────────────────────────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,

    // ── Sysreg interface (one per cache instance) ──────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Derived parameters
    // ══════════════════════════════════════════════════════════
    localparam WORD_BITS = $clog2(LINE_WORDS);
    localparam SET_BITS  = $clog2(NUM_SETS);
    localparam WORD_LSB  = 2;                      // bits [1:0] = byte offset
    localparam SET_LSB   = WORD_LSB + WORD_BITS;
    localparam TAG_LSB   = SET_LSB + SET_BITS;
    localparam TAG_BITS  = 32 - TAG_LSB;

    // ══════════════════════════════════════════════════════════
    // Address field extraction
    // ══════════════════════════════════════════════════════════
    logic [WORD_BITS-1:0] addr_word;
    logic [SET_BITS-1:0]  addr_set;
    logic [TAG_BITS-1:0]  addr_tag;

    assign addr_word = i_paddr[WORD_LSB +: WORD_BITS];
    assign addr_set  = i_paddr[SET_LSB  +: SET_BITS];
    assign addr_tag  = i_paddr[TAG_LSB  +: TAG_BITS];

    // ══════════════════════════════════════════════════════════
    // Cache storage
    // ══════════════════════════════════════════════════════════
    logic                valid [0:NUM_SETS-1];
    logic [TAG_BITS-1:0] tags  [0:NUM_SETS-1];
    logic [31:0]         data  [0:NUM_SETS*LINE_WORDS-1];

    function automatic int data_idx(
        input logic [SET_BITS-1:0]  s,
        input logic [WORD_BITS-1:0] w
    );
        return int'(s) * LINE_WORDS + int'(w);
    endfunction

    // ══════════════════════════════════════════════════════════
    // Hit detection (combinational)
    // ══════════════════════════════════════════════════════════
    logic hit;
    assign hit = valid[addr_set] && (tags[addr_set] == addr_tag);

    // Cache active = enabled AND cacheable access
    logic cache_en;
    logic cache_active;
    assign cache_active = cache_en && i_cacheable;

    // ══════════════════════════════════════════════════════════
    // Sysreg: control and info registers
    // ══════════════════════════════════════════════════════════
    logic inval_req;

    localparam logic [31:0] INFO_VALUE = {
        10'b0,
        CACHE_TYPE[3:0],
        NUM_WAYS[3:0],
        NUM_SETS[9:0],
        LINE_WORDS[3:0]
    };

    always_comb begin
        case (i_sys_reg)
            SYSREG_CACHE_INFO:  o_sys_rdata = INFO_VALUE;
            SYSREG_CACHE_CTRL:  o_sys_rdata = {31'b0, cache_en};
            default:            o_sys_rdata = 32'b0;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cache_en  <= 1'b0;
            inval_req <= 1'b0;
        end else begin
            inval_req <= 1'b0;
            if (i_sys_we) begin
                case (i_sys_reg)
                    SYSREG_CACHE_CTRL:  cache_en  <= i_sys_wdata[0];
                    SYSREG_CACHE_INVAL: inval_req <= 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // State machine — only S_FILL needs a state
    // ══════════════════════════════════════════════════════════
    typedef enum logic {
        S_IDLE,
        S_FILL
    } state_t;

    state_t state;

    // Fill state
    logic [31:0]          fill_base_addr;  // Line-aligned base address
    logic [TAG_BITS-1:0]  fill_tag;
    logic [SET_BITS-1:0]  fill_set;
    logic [WORD_BITS-1:0] fill_count;      // Current word being filled
    logic                 fill_req;        // Requesting this word from memory
    logic                 fill_wait;       // Waiting for memory to respond

    // ══════════════════════════════════════════════════════════
    // Memory bus output mux
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // Defaults
        o_mem_addr    = i_paddr;
        o_mem_wdata   = i_wdata;
        o_mem_byte_en = i_byte_en;
        o_mem_we      = 1'b0;
        o_mem_re      = 1'b0;

        case (state)
            S_IDLE: begin
                if (cache_active && i_re && hit) begin
                    // Read hit: no memory access
                end else if (cache_active && i_re && !hit) begin
                    // Read miss: don't access memory here — S_FILL handles it
                end else begin
                    // Pass through: uncacheable, disabled, or any write
                    o_mem_re = i_re;
                    o_mem_we = i_we;
                end
            end

            S_FILL: begin
                o_mem_addr = {fill_base_addr[31:WORD_LSB+WORD_BITS],
                              fill_count, {WORD_LSB{1'b0}}};
                // Hold i_re for the entire read (request + wait).
                // simple_mem needs i_re asserted continuously so its
                // busy protocol works: o_busy = (i_re||i_we) && !done.
                o_mem_re   = fill_req || fill_wait;
            end
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Read data output
    // ══════════════════════════════════════════════════════════
    always_comb begin
        if (state == S_IDLE && cache_active && i_re && hit)
            o_rdata = data[data_idx(addr_set, addr_word)];
        else
            o_rdata = i_mem_rdata;
    end

    // ══════════════════════════════════════════════════════════
    // Busy signal
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (state)
            S_IDLE: begin
                if (cache_active && i_re && hit)
                    o_busy = 1'b0;          // Cache hit: instant
                else if (cache_active && i_re && !hit)
                    o_busy = 1'b1;          // Read miss: will fill
                else
                    o_busy = i_mem_busy;    // Pass-through (writes, uncacheable)
            end
            S_FILL:
                o_busy = 1'b1;              // Fill in progress
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // State machine and storage updates
    // ══════════════════════════════════════════════════════════
    integer i;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state      <= S_IDLE;
            fill_count <= '0;
            fill_req   <= 1'b0;
            fill_wait  <= 1'b0;
            fill_base_addr <= 32'b0;
            fill_tag   <= '0;
            fill_set   <= '0;
            for (i = 0; i < NUM_SETS; i++)
                valid[i] <= 1'b0;
        end else begin

            // ── Invalidate (priority over other operations) ──
            if (inval_req) begin
                for (i = 0; i < NUM_SETS; i++)
                    valid[i] <= 1'b0;
            end

            case (state)
                S_IDLE: begin
                    // ── Write hit: update cache line (byte-granular) ──
                    if (cache_active && i_we && hit) begin
                        if (i_byte_en[0])
                            data[data_idx(addr_set, addr_word)][ 7: 0] <= i_wdata[ 7: 0];
                        if (i_byte_en[1])
                            data[data_idx(addr_set, addr_word)][15: 8] <= i_wdata[15: 8];
                        if (i_byte_en[2])
                            data[data_idx(addr_set, addr_word)][23:16] <= i_wdata[23:16];
                        if (i_byte_en[3])
                            data[data_idx(addr_set, addr_word)][31:24] <= i_wdata[31:24];
                    end

                    // ── Read miss: start line fill ──────────────
                    if (cache_active && i_re && !hit) begin
                        fill_base_addr <= i_paddr;
                        fill_tag       <= addr_tag;
                        fill_set       <= addr_set;
                        fill_count     <= '0;
                        fill_req       <= 1'b1;
                        fill_wait      <= 1'b0;
                        state          <= S_FILL;
                    end
                end

                S_FILL: begin
                    if (fill_req && !fill_wait) begin
                        // Memory sees our request this cycle.
                        // Transition to waiting for completion.
                        fill_wait <= 1'b1;
                        fill_req  <= 1'b0;
                    end else if (fill_wait && !i_mem_busy) begin
                        // Memory completed — store word
                        data[data_idx(fill_set, fill_count)] <= i_mem_rdata;

                        if (fill_count == WORD_BITS'(LINE_WORDS - 1)) begin
                            // Last word: install tag, set valid
                            tags[fill_set]  <= fill_tag;
                            valid[fill_set] <= 1'b1;
                            fill_wait       <= 1'b0;
                            state           <= S_IDLE;
                        end else begin
                            fill_count <= fill_count + WORD_BITS'(1);
                            fill_req   <= 1'b1;
                            fill_wait  <= 1'b0;
                        end
                    end
                end
            endcase
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
