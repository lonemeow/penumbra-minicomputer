// penumbra3_load_complete -- registered load-completion / hold buffer.
//
// The back-end's single load-completion point, and the module that keeps
// the cache hit/busy verdict off the issue and MMU cones (the gen2 floor).
// The MEM2 verdict is a combinational input consumed *here* into a flop; the
// pipeline's only view of it is the registered o_load_pending. A miss parks
// the load in the one-entry hold buffer (single-outstanding), the line fill
// is launched once, and a registered completion delivers {data|fault} to
// the held -- and therefore oldest -- load.
module penumbra3_load_complete #(
    parameter int IDX_BITS = 5      // destination scoreboard-index width
) (
    input  logic                i_clk,
    input  logic                i_rst,

    // MEM2 verdict for a load (combinational; consumed into a flop)
    input  logic                i_load_valid,   // a load is resolving in MEM2
    input  logic                i_cache_hit,
    input  logic [IDX_BITS-1:0] i_load_dest,

    // Line-fill response (registered completion from the bus master)
    input  logic                i_fill_done,
    input  logic [31:0]         i_fill_data,
    input  logic                i_fill_fault,

    // Registered pipeline gate + completion
    output logic                o_load_pending, // gates the pipe hold
    output logic                o_launch_fill,  // kick the line fill (1-cycle)
    output logic                o_complete,     // a held load finished (1-cycle)
    output logic [IDX_BITS-1:0] o_complete_dest,
    output logic [31:0]         o_complete_data,
    output logic                o_complete_fault
);

    typedef enum logic {L_IDLE, L_PENDING} state_e;

    state_e              state_q;
    logic [IDX_BITS-1:0] dest_q;
    logic                launch_fill_q;
    logic                complete_q;
    logic [IDX_BITS-1:0] complete_dest_q;
    logic [31:0]         complete_data_q;
    logic                complete_fault_q;

    // A miss enters the hold: a load resolving in MEM2 that did not hit.
    logic miss_enter;
    assign miss_enter = (state_q == L_IDLE) && i_load_valid && !i_cache_hit;

    assign o_load_pending   = (state_q == L_PENDING);
    assign o_launch_fill    = launch_fill_q;
    assign o_complete       = complete_q;
    assign o_complete_dest  = complete_dest_q;
    assign o_complete_data  = complete_data_q;
    assign o_complete_fault = complete_fault_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q          <= L_IDLE;
            dest_q           <= '0;
            launch_fill_q    <= 1'b0;
            complete_q       <= 1'b0;
            complete_dest_q  <= '0;
            complete_data_q  <= '0;
            complete_fault_q <= 1'b0;
        end else begin
            launch_fill_q <= miss_enter;   // kick the fill the cycle we park
            complete_q    <= 1'b0;

            if (miss_enter) begin
                state_q <= L_PENDING;
                dest_q  <= i_load_dest;
            end else if (state_q == L_PENDING && i_fill_done) begin
                state_q          <= L_IDLE;
                complete_q       <= 1'b1;
                complete_dest_q  <= dest_q;
                complete_data_q  <= i_fill_data;
                complete_fault_q <= i_fill_fault;
            end
        end
    end

    // Single-outstanding: a fill completion only arrives for the load held in
    // L_PENDING, so a done pulse while idle is a bus-protocol violation.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(state_q == L_IDLE && i_fill_done))
        else $error("penumbra3_load_complete: fill completion with no load pending");

endmodule
