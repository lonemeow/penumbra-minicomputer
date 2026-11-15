// penumbra3_mem_completion -- the data side's completion unit.
//
// The back-end's single completion point for the accesses MEM2 cannot
// finish. A load the L1-D had no word for and an uncached store are parked
// here as they leave MEM2, in the cycle the L1-D takes the access
// (doc/internals/penumbra3/l1-cache.md). The unit keeps what the
// instruction has left the pipe with, and completes the access when the
// L1-D says it is done: a load's word for the regfile, or a bus fault
// taken precisely. A store's write is its whole effect. The instruction
// retires with its completion. The pipeline waits on the L1-D's registered
// busy, which freezes the launch side (MEM1, the MEM1/MEM2 register, the
// translate and L1 launches, issue) from the cycle after the park.
//
// One access, cycle by cycle. p is the park, d the L1-D's done:
//
//   p     i_park: the access's descriptor is latched
//   p+1   the L1-D is busy; the descriptor is held
//   d     o_complete, with the regfile write for a load. o_complete_fault
//         if the access faulted
//
// The descriptor is what the instruction needs then: a load's destination
// and sub-word extract (size, sign, the address's low bits), and the PC,
// the virtual address and the mode a bus fault reports.
module penumbra3_mem_completion
    import penumbra_pkg::*;
#(
    parameter  int IDX_BITS   = 5       // destination scoreboard-index width
) (
    input  logic                i_clk,
    input  logic                i_rst,

    // -- Park: an access MEM2 cannot finish leaves MEM2 this cycle --
    input  logic                i_park,           // one cycle; never while i_l1_busy
    input  logic                i_access_we,      // a store
    input  logic [31:0]         i_access_pc,
    input  logic [31:0]         i_access_vaddr,
    input  logic                i_access_user,    // the instruction ran in user mode
    input  logic [IDX_BITS-1:0] i_load_dest,
    input  logic [1:0]          i_load_size,
    input  logic                i_load_sign,

    // -- The L1-D's access ----------------------------------------
    input  logic                i_l1_busy,
    input  logic                i_l1_done,        // one cycle
    input  logic [31:0]         i_l1_word,        // valid from i_l1_done
    input  logic                i_l1_fault,       // valid from i_l1_done

    // -- Completion -----------------------------------------------
    output logic                o_pending_store,  // with i_l1_busy: a store
    output logic                o_complete,       // the access finished (one cycle)
    output logic                o_complete_we,    // with o_complete: a load
    output logic [IDX_BITS-1:0] o_complete_dest,
    output logic [31:0]         o_complete_data,  // the whole word; the consumer extracts
    output logic [1:0]          o_complete_size,
    output logic [1:0]          o_complete_addr_lo,
    output logic                o_complete_sign,

    // -- A bus fault ----------------------------------------------
    // A cacheable store never parks: it retires into the store buffer, whose
    // writes fault on store_fault (uncore-port.md, "Faults"). So every fault
    // reported here is precise.
    output logic                o_complete_fault, // one cycle, with o_complete
    output logic [31:0]         o_complete_pc,
    output logic [31:0]         o_complete_vaddr,
    output logic [31:0]         o_complete_status
);

    // ── The parked access ────────────────────────────────────────
    // Loaded every cycle the L1-D is not busy, so only the L1-D's request
    // register carries the park decision, which waits on the TLB verdict
    // and the L1 hit; these registers' enables stay off it. From p+1 they
    // hold the parked access through the done, where the completion reads
    // them.
    logic                we_q;
    logic [31:0]         pc_q;
    logic [31:0]         vaddr_q;
    logic                user_q;
    logic [IDX_BITS-1:0] dest_q;
    logic [1:0]          size_q;
    logic                sign_q;

    always_ff @(posedge i_clk)
        if (!i_l1_busy) begin
            we_q        <= i_access_we;
            pc_q        <= i_access_pc;
            vaddr_q     <= i_access_vaddr;
            user_q      <= i_access_user;
            dest_q      <= i_load_dest;
            size_q      <= i_load_size;
            sign_q      <= i_load_sign;
        end

    assign o_pending_store = we_q;

    // ── Completion ───────────────────────────────────────────────
    // The page offset is the same in the virtual and the physical address,
    // so the extract takes its low bits from the virtual one.
    assign o_complete         = i_l1_done;
    assign o_complete_we      = !we_q;
    assign o_complete_dest    = dest_q;
    assign o_complete_data    = i_l1_word;
    assign o_complete_size    = size_q;
    assign o_complete_addr_lo = vaddr_q[1:0];
    assign o_complete_sign    = sign_q;

    assign o_complete_fault  = i_l1_done && i_l1_fault;
    assign o_complete_pc     = pc_q;
    assign o_complete_vaddr  = vaddr_q;
    assign o_complete_status = compose_fault_status(user_q, we_q ? ACC_WRITE : ACC_READ,
                                                    FAULT_BUS);

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // One access at a time: the freeze keeps a second from reaching MEM2's
    // exit while one is in flight.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_park && i_l1_busy))
        else $error("penumbra3_mem_completion: an access parked while one is in flight");

endmodule
