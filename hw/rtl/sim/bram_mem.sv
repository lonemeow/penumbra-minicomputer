// bram_mem — pure block-RAM model: registered read, sustained 1 access/cycle.
//
// Models the contract a BRAM-backed cache presents to the pipeline on a hit
// (ECP5 BRAM with REGMODE_A=NOREG): the address is sampled at the clock edge
// and the addressed word appears combinationally during the *next* cycle,
// every cycle. Drive a new address each cycle and a new word comes out each
// cycle — there is no busy handshake.
//
// This is deliberately a different contract from simple_mem / fpga_ram, which
// model the SDRAM *bus* protocol: those assert a one-cycle busy and need a
// turnaround cycle between accesses, so back-to-back reads sustain only one
// per two cycles. The gen2 IF1/IF2 split is built around the streaming BRAM
// contract (IF1 drives the address, IF2 consumes the word the following
// cycle, one fetch per cycle), so it needs this model, not those. The
// consumer pairs each returned word with the PC carried alongside it through
// the IF1/IF2 register; nothing here tracks which access a word belongs to.
//
// The read has a clock-enable (i_en), modelling the ECP5 BRAM output-register
// CE pin. While i_en is low the output register holds its current word — the
// consumer asserts this to freeze the read in lockstep with a stalled
// pipeline stage. Without it, a registered-read RAM behind a stall delivers
// the *next* word (whose read was already in flight when the stall began)
// against the held address, dropping an instruction.
//
// Byte-enabled synchronous write, no read/write-through (a read in the same
// cycle a word is written returns the old value). Word-addressed, wraps
// modulo MEM_WORDS. Optional $readmemh init for standalone program-runner
// testbenches.

// Word-addressed: the high address bits beyond MEM_WORDS and the low two
// byte-select bits of i_addr are intentionally unused (the address wraps).
// verilator lint_off UNUSEDSIGNAL

module bram_mem #(
    parameter int    MEM_WORDS = 4096,
    parameter string INIT_FILE = ""
)(
    input  logic        i_clk,
    input  logic [31:0] i_addr,     // byte address (wraps modulo MEM_WORDS)
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,  // per-byte write enables (active high)
    input  logic        i_we,
    input  logic        i_en,       // read clock-enable: hold the output when low
    output logic [31:0] o_rdata     // addressed word, valid the cycle after i_addr
);

    localparam int ADDR_BITS = $clog2(MEM_WORDS);

    logic [31:0] mem [0:MEM_WORDS-1];

    initial begin
        for (int i = 0; i < MEM_WORDS; i++)
            mem[i] = 32'b0;
        if (INIT_FILE != "")
            $readmemh(INIT_FILE, mem);
    end

    logic [ADDR_BITS-1:0] word_addr;
    assign word_addr = i_addr[ADDR_BITS+1:2];

    // ── Synchronous byte-enabled write ──────────────────────
    always_ff @(posedge i_clk) begin
        if (i_we) begin
            if (i_byte_en[0]) mem[word_addr][ 7: 0] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem[word_addr][15: 8] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem[word_addr][23:16] <= i_wdata[23:16];
            if (i_byte_en[3]) mem[word_addr][31:24] <= i_wdata[31:24];
        end
    end

    // ── Registered read: addressed word appears next cycle while enabled ──
    // i_en low holds the output, keeping it aligned with a frozen consumer.
    always_ff @(posedge i_clk)
        if (i_en) o_rdata <= mem[word_addr];

endmodule

// verilator lint_on UNUSEDSIGNAL
