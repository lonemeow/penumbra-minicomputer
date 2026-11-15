// penumbra3_store_buffer -- cacheable stores between MEM2 and the uncore.
//
// A cacheable store completes as it enters the buffer and drains through
// the uncore in the background (doc/internals/penumbra3/memory-performance.md,
// "Store buffer at the core boundary"). An entry holds one store -- its
// word's physical address, its data and its byte lanes -- and passes
// through three states in order:
//
//   queued  L2 and memory have not seen it. The oldest is offered to the
//           uncore (o_post_*), whose verdict posts it: writes it into L2 if
//           L2 holds its line, and owes memory the write.
//   posted  its bus write is owed. The oldest is offered to the bus master
//           (o_bus_*), which takes it ahead of any other bus transaction.
//   free    its bus write is done.
//
// Entries keep the order they entered in, so the states are runs of a
// ring: [head, post) posted, [post, tail) queued. A store is registered as
// it enters (the input stage) and written into the ring a cycle later, so
// MEM2's address reaches nothing but that register; it counts as queued
// from the cycle it enters.
//
// The uncore orders its other operations by the buffer:
//   - a line read waits while a queued store holds its line
//     (o_line_queued), so L2 holds every older store to the line when it
//     serves the read; a read that goes to memory follows every posted
//     write, which the bus master takes first (o_posted);
//   - a word access waits for the buffer to be empty (o_empty), so it
//     reaches the bus after every earlier store.
//
// o_full is registered and exact: low means a store entering this cycle
// finds an entry, whatever else the cycle does.
module penumbra3_store_buffer #(
    parameter  unsigned ENTRIES  = 16,
    localparam unsigned IDX_BITS = $clog2(ENTRIES),
    localparam unsigned CNT_BITS = $clog2(ENTRIES + 1)
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Enqueue: a cacheable store leaving MEM2 ──────────────────
    input  logic        i_enq,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_enq_addr,      // physical; the word selects
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_enq_wdata,     // each byte in its lane
    input  logic [3:0]  i_enq_byte_en,
    output logic        o_full,          // registered: no store may enter this cycle

    // ── Post: the oldest queued store, to the uncore's select ────
    output logic        o_post_valid,
    output logic [31:0] o_post_addr,
    output logic [31:0] o_post_wdata,
    output logic [3:0]  o_post_byte_en,
    input  logic        i_posted,        // the uncore's verdict posted it

    // ── Bus: the oldest posted store, to the bus master ──────────
    output logic        o_bus_valid,     // the bus master has not taken it
    output logic [31:0] o_bus_addr,
    output logic [31:0] o_bus_wdata,
    output logic [3:0]  o_bus_byte_en,
    input  logic        i_bus_taken,
    input  logic        i_bus_done,      // its write completed: the entry frees
    output logic        o_posted,        // a posted store still owes its write

    // ── Ordering ─────────────────────────────────────────────────
    input  logic [31:4] i_line,          // a line read's line
    output logic        o_line_queued,   // a queued store holds it
    output logic        o_empty
);

    build_check #(
        .COND ((1 << IDX_BITS) == ENTRIES),
        .MSG  ("penumbra3_store_buffer: ENTRIES must be a power of two")
    ) u_check_entries ();

    typedef struct packed {
        logic [31:2] addr;
        logic [31:0] wdata;
        logic [3:0]  byte_en;
    } entry_t;
    localparam int ENTRY_BITS = $bits(entry_t);

    // ── The input stage ──────────────────────────────────────────
    // Only the valid bit carries i_enq, which waits on MEM2's translation
    // verdict; the store loads every cycle and is read only while valid.
    logic   in_valid_q;
    entry_t in_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) in_valid_q <= 1'b0;
        else       in_valid_q <= i_enq;
        in_q <= '{addr: i_enq_addr[31:2], wdata: i_enq_wdata, byte_en: i_enq_byte_en};
    end

    // ── The ring ─────────────────────────────────────────────────
    // A copy for each read port, as LUT RAM has one read.
    (* ram_style = "distributed" *) logic [ENTRY_BITS-1:0] ring_post [ENTRIES];
    (* ram_style = "distributed" *) logic [ENTRY_BITS-1:0] ring_bus  [ENTRIES];

    typedef logic [IDX_BITS-1:0] idx_t;
    logic write, post, free;
    idx_t tail_q, posting_q, head_q;   // next free, oldest queued, oldest posted
    assign write = in_valid_q;      // the input stage's store goes into the ring
    assign post  = i_posted;
    assign free  = i_bus_done;

    always_ff @(posedge i_clk) begin
        if (write) begin
            ring_post[tail_q] <= in_q;
            ring_bus[tail_q]  <= in_q;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tail_q    <= '0;
            posting_q <= '0;
            head_q    <= '0;
        end else begin
            if (write) tail_q    <= tail_q + 1'b1;
            if (post)  posting_q <= posting_q + 1'b1;
            if (free)  head_q    <= head_q + 1'b1;
        end
    end

    // ── Counts ───────────────────────────────────────────────────
    // One for each thing an output reports, stepped by the two events that
    // begin and end it; the outputs register a compare of its next value.
    //   taken        entries in use: a store enters, its write completes.
    //                The input stage's store holds an entry.
    //   ring_queued  queued stores in the ring, which the post port offers:
    //                written into the ring, posted
    //   posted       posted stores: posted, written
    //
    // i_enq waits on MEM2's translation verdict; i_bus_done comes from the
    // bus master's state. So the taken count settles the free first (held)
    // and i_enq only chooses between results already computed from it, here
    // and for full and empty.
    typedef logic [CNT_BITS-1:0] count_t;
    localparam count_t COUNT_FULL      = count_t'(ENTRIES);
    localparam count_t COUNT_ONE_SHORT = count_t'(ENTRIES - 1);
    count_t taken_count_q,       taken_count_d, held;
    count_t ring_queued_count_q, ring_queued_count_d;
    count_t posted_count_q,      posted_count_d;
    assign held                = taken_count_q - count_t'(free);
    assign taken_count_d       = i_enq ? held + 1'b1 : held;
    assign ring_queued_count_d = ring_queued_count_q + count_t'(write) - count_t'(post);
    assign posted_count_d      = posted_count_q      + count_t'(post)  - count_t'(free);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            taken_count_q       <= '0;
            ring_queued_count_q <= '0;
            posted_count_q      <= '0;
        end else begin
            taken_count_q       <= taken_count_d;
            ring_queued_count_q <= ring_queued_count_d;
            posted_count_q      <= posted_count_d;
        end
    end

    // ── The post port ────────────────────────────────────────────
    // Registered: the oldest queued store after this cycle's post. That is
    // the store entering the ring this cycle when nothing older is queued.
    idx_t   post_next;
    entry_t post_entry;
    logic   post_valid_q;
    entry_t post_out_q;
    assign post_next  = posting_q + idx_t'(post);
    assign post_entry = (write && post_next == tail_q) ? in_q : entry_t'(ring_post[post_next]);

    always_ff @(posedge i_clk) begin
        if (i_rst) post_valid_q <= 1'b0;
        else       post_valid_q <= (ring_queued_count_d != '0);
        post_out_q <= post_entry;
    end

    assign o_post_valid   = post_valid_q;
    assign o_post_addr    = {post_out_q.addr, 2'b00};
    assign o_post_wdata   = post_out_q.wdata;
    assign o_post_byte_en = post_out_q.byte_en;

    // ── The bus port ─────────────────────────────────────────────
    // Registered: the oldest posted store after this cycle's free. A posted
    // store entered the ring cycles before it posted, so the ring holds it.
    idx_t   head_next;
    logic   sent_q, sent_d, bus_valid_q, posted_q;
    entry_t bus_out_q;
    assign head_next = head_q + idx_t'(free);
    assign sent_d    = !free && (sent_q || i_bus_taken);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            sent_q      <= 1'b0;
            bus_valid_q <= 1'b0;
            posted_q    <= 1'b0;
        end else begin
            sent_q      <= sent_d;
            bus_valid_q <= (posted_count_d != '0) && !sent_d;
            posted_q    <= (posted_count_d != '0);
        end
        bus_out_q <= entry_t'(ring_bus[head_next]);
    end

    assign o_bus_valid   = bus_valid_q;
    assign o_bus_addr    = {bus_out_q.addr, 2'b00};
    assign o_bus_wdata   = bus_out_q.wdata;
    assign o_bus_byte_en = bus_out_q.byte_en;
    assign o_posted      = posted_q;

    // ── The line search ──────────────────────────────────────────
    // Every queued store's line, in registers, for a compare against all of
    // them at once: the input stage's, and each ring entry's from its write
    // until it posts. Each entry is written through a decode of the
    // pointers: an index into the packed array synthesizes as a shifter.
    logic [ENTRIES-1:0]        queued_e_q;
    logic [ENTRIES-1:0][31:4]  line_e_q;
    always_ff @(posedge i_clk) begin
        for (int e = 0; e < ENTRIES; e++) begin
            if (i_rst)
                queued_e_q[e] <= 1'b0;
            else if (write && tail_q == idx_t'(e))
                queued_e_q[e] <= 1'b1;
            else if (post && posting_q == idx_t'(e))
                queued_e_q[e] <= 1'b0;
            if (write && tail_q == idx_t'(e))
                line_e_q[e] <= in_q.addr[31:4];
        end
    end

    always_comb begin
        o_line_queued = in_valid_q && in_q.addr[31:4] == i_line;
        for (int e = 0; e < ENTRIES; e++)
            o_line_queued |= queued_e_q[e] && line_e_q[e] == i_line;
    end

    // ── Full and empty ───────────────────────────────────────────
    logic full_q, empty_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            full_q  <= 1'b0;
            empty_q <= 1'b1;
        end else begin
            full_q  <= i_enq ? (held == COUNT_ONE_SHORT) : (held == COUNT_FULL);
            empty_q <= !i_enq && (held == '0);
        end
    end

    assign o_full  = full_q;
    assign o_empty = empty_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst) begin
            assert (!(i_enq && o_full))
                else $error("penumbra3_store_buffer: a store entered a full buffer");
            assert (!(i_posted && !o_post_valid))
                else $error("penumbra3_store_buffer: posted with nothing queued");
            assert (!(i_bus_taken && !o_bus_valid))
                else $error("penumbra3_store_buffer: the bus took nothing");
            assert (!(i_bus_done && !sent_q))
                else $error("penumbra3_store_buffer: a write completed that was never taken");
            assert (taken_count_q == ring_queued_count_q + posted_count_q + count_t'(in_valid_q))
                else $error("penumbra3_store_buffer: the counts disagree on the entries taken");
        end
    end

endmodule
