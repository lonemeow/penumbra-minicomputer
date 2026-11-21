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
//           L2 holds its line, and owes memory the bus write.
//   posted  its bus write is owed, in one of two states:
//             unsent  the oldest is offered to the bus master (o_bus_*),
//                     which takes it ahead of any other bus transaction;
//             sent    the bus master has taken it, and its bus write has
//                     not completed. The master may take a store while
//                     earlier ones are still sent.
//   free    its bus write has completed.
//
// Entries keep the order they entered in, so the states are runs of a
// ring, oldest first: the sent stores, the unsent ones from send, and the
// queued ones from posting up to tail. The free end needs no pointer, as
// nothing reads a store once its bus write completes. A store is
// registered as it enters (the input stage) and goes into the ring a
// cycle later (the ring write), so MEM2's address reaches nothing but
// that register; it counts as queued from the cycle it enters.
//
// The uncore orders its other operations by the buffer:
//   - a line read waits while a queued store holds its line
//     (o_line_queued), so L2 holds every older store to the line when it
//     serves the read; a read that goes to memory follows every posted
//     store's bus write, which the bus master takes first (o_posted);
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

    // ── Bus: the oldest unsent store, to the bus master ──────────
    output logic        o_bus_valid,     // an unsent store is offered
    output logic [31:0] o_bus_addr,
    output logic [31:0] o_bus_wdata,
    output logic [3:0]  o_bus_byte_en,
    input  logic        i_bus_taken,     // the bus master took the offered store: it is sent
    input  logic        i_bus_done,      // the oldest sent store's bus write completed: it frees
    output logic        o_posted,        // a posted store still owes its bus write

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

    // ── Events ───────────────────────────────────────────────────
    // What a cycle may do to the stores in the ring; each event steps a
    // pointer, a count, or both.
    logic ring_write, post, send, free;
    assign ring_write = in_valid_q;    // the input stage's store goes into the ring
    assign post       = i_posted;      // the oldest queued store posts
    assign send       = i_bus_taken;   // the oldest unsent store is sent
    assign free       = i_bus_done;    // the oldest sent store's bus write completes

    // ── The ring ─────────────────────────────────────────────────
    // A copy for each read port, as LUT RAM has one read. Each port reads
    // at its pointer's next value, so it registers the store it offers
    // after this cycle's events.
    (* ram_style = "distributed" *) logic [ENTRY_BITS-1:0] ring_post [ENTRIES];
    (* ram_style = "distributed" *) logic [ENTRY_BITS-1:0] ring_bus  [ENTRIES];

    typedef logic [IDX_BITS-1:0] idx_t;
    idx_t tail_q,    tail_d;       // the entry the next ring write fills
    idx_t posting_q, posting_d;    // the oldest queued store
    idx_t send_q,    send_d;       // the oldest unsent store
    assign tail_d    = tail_q    + idx_t'(ring_write);
    assign posting_d = posting_q + idx_t'(post);
    assign send_d    = send_q    + idx_t'(send);

    always_ff @(posedge i_clk) begin
        if (ring_write) begin
            ring_post[tail_q] <= in_q;
            ring_bus[tail_q]  <= in_q;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tail_q    <= '0;
            posting_q <= '0;
            send_q    <= '0;
        end else begin
            tail_q    <= tail_d;
            posting_q <= posting_d;
            send_q    <= send_d;
        end
    end

    // ── Counts ───────────────────────────────────────────────────
    // One for each thing an output reports, stepped by the two events that
    // begin and end it; the outputs register a compare of its next value.
    //   used         entries in use: a store enters, free. The input
    //                stage's store holds an entry.
    //   ring_queued  queued stores in the ring, which the post port
    //                offers: ring_write, post
    //   posted       posted stores, sent or not: post, free
    //   unsent       unsent stores, which the bus port offers: post, send
    //
    // i_enq waits on MEM2's translation verdict; i_bus_done comes from the
    // bus master's state. So the used count settles the free first
    // (used_kept) and i_enq only chooses between results already computed
    // from it, here and for full and empty.
    typedef logic [CNT_BITS-1:0] count_t;
    localparam count_t COUNT_FULL      = count_t'(ENTRIES);
    localparam count_t COUNT_ONE_SHORT = count_t'(ENTRIES - 1);
    count_t used_count_q,        used_count_d, used_kept;
    count_t ring_queued_count_q, ring_queued_count_d;
    count_t posted_count_q,      posted_count_d;
    count_t unsent_count_q,      unsent_count_d;
    assign used_kept           = used_count_q - count_t'(free);
    assign used_count_d        = i_enq ? used_kept + 1'b1 : used_kept;
    assign ring_queued_count_d = ring_queued_count_q + count_t'(ring_write) - count_t'(post);
    assign posted_count_d      = posted_count_q      + count_t'(post)       - count_t'(free);
    assign unsent_count_d      = unsent_count_q      + count_t'(post)       - count_t'(send);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            used_count_q        <= '0;
            ring_queued_count_q <= '0;
            posted_count_q      <= '0;
            unsent_count_q      <= '0;
        end else begin
            used_count_q        <= used_count_d;
            ring_queued_count_q <= ring_queued_count_d;
            posted_count_q      <= posted_count_d;
            unsent_count_q      <= unsent_count_d;
        end
    end

    // ── The post port ────────────────────────────────────────────
    // Registered: the oldest queued store after this cycle's post. That is
    // the store entering the ring this cycle when nothing older is queued.
    logic   post_valid_q;
    entry_t post_entry, post_out_q;
    assign post_entry = (ring_write && posting_d == tail_q) ? in_q : entry_t'(ring_post[posting_d]);

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
    // Registered: the oldest unsent store after this cycle's events. A
    // posted store entered the ring cycles before it posted, so the ring
    // holds it. The master may take it while earlier stores are still
    // sent, as the oldest sent store's bus write completes, so a send and a
    // free can share a cycle; each entry frees only with its own bus write.
    logic   bus_valid_q;
    entry_t bus_out_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) bus_valid_q <= 1'b0;
        else       bus_valid_q <= (unsent_count_d != '0);
        bus_out_q <= entry_t'(ring_bus[send_d]);
    end

    assign o_bus_valid   = bus_valid_q;
    assign o_bus_addr    = {bus_out_q.addr, 2'b00};
    assign o_bus_wdata   = bus_out_q.wdata;
    assign o_bus_byte_en = bus_out_q.byte_en;

    // ── The line search ──────────────────────────────────────────
    // Every queued store's line, in registers, for a compare against all of
    // them at once: the input stage's, and each ring entry's from its ring
    // write until it posts. Each entry's registers load through a decode of
    // the pointers: an index into the packed array synthesizes as a shifter.
    logic [ENTRIES-1:0]        queued_e_q;
    logic [ENTRIES-1:0][31:4]  line_e_q;
    always_ff @(posedge i_clk) begin
        for (int e = 0; e < ENTRIES; e++) begin
            if (i_rst)
                queued_e_q[e] <= 1'b0;
            else if (ring_write && tail_q == idx_t'(e))
                queued_e_q[e] <= 1'b1;
            else if (post && posting_q == idx_t'(e))
                queued_e_q[e] <= 1'b0;
            if (ring_write && tail_q == idx_t'(e))
                line_e_q[e] <= in_q.addr[31:4];
        end
    end

    always_comb begin
        o_line_queued = in_valid_q && in_q.addr[31:4] == i_line;
        for (int e = 0; e < ENTRIES; e++)
            o_line_queued |= queued_e_q[e] && line_e_q[e] == i_line;
    end

    // ── Full, empty and posted ───────────────────────────────────
    logic full_q, empty_q, posted_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            full_q   <= 1'b0;
            empty_q  <= 1'b1;
            posted_q <= 1'b0;
        end else begin
            full_q   <= i_enq ? (used_kept == COUNT_ONE_SHORT) : (used_kept == COUNT_FULL);
            empty_q  <= !i_enq && (used_kept == '0);
            posted_q <= (posted_count_d != '0);
        end
    end

    assign o_full   = full_q;
    assign o_empty  = empty_q;
    assign o_posted = posted_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst) begin
            assert (!(i_enq && o_full))
                else $error("penumbra3_store_buffer: a store entered a full buffer");
            assert (!(i_posted && !o_post_valid))
                else $error("penumbra3_store_buffer: posted with nothing queued");
            assert (!(i_bus_taken && !o_bus_valid))
                else $error("penumbra3_store_buffer: the bus took nothing");
            assert (!(i_bus_done && posted_count_q == unsent_count_q))
                else $error("penumbra3_store_buffer: a write completed that was never sent");
            assert (used_count_q == ring_queued_count_q + posted_count_q + count_t'(in_valid_q))
                else $error("penumbra3_store_buffer: the counts disagree on the entries in use");
        end
    end

endmodule
