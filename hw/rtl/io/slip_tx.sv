// SLIP transmit encoder — adds RFC 1055 framing to a byte stream
//
// Accepts payload bytes (i_valid/i_data) and produces SLIP-encoded
// output (o_valid/o_data).  When a payload byte needs escaping (0xC0
// or 0xDB), the encoder asserts o_busy for one cycle while it sends
// the two-byte escape sequence.
//
// i_frame_end requests an END delimiter.  The encoder sends END and
// returns to idle.
//
// SLIP special bytes:
//   END  = 0xC0 — frame delimiter
//   ESC  = 0xDB — escape prefix
//   0xDC after ESC — literal 0xC0
//   0xDD after ESC — literal 0xDB

module slip_tx
    import penumbra_pkg::*;
(
    input  logic       i_clk,
    input  logic       i_rst,

    // Payload input
    input  logic       i_valid,      // payload byte available
    input  logic [7:0] i_data,       // payload byte
    input  logic       i_frame_end,  // request END delimiter

    // Back-pressure to upstream
    output logic       o_busy,       // high: don't send next byte yet

    // Encoded byte output (to UART TX)
    output logic       o_valid,      // encoded byte available
    output logic [7:0] o_data        // encoded byte
);

    localparam logic [7:0] SLIP_END     = 8'hC0;
    localparam logic [7:0] SLIP_ESC     = 8'hDB;
    localparam logic [7:0] SLIP_ESC_END = 8'hDC;
    localparam logic [7:0] SLIP_ESC_ESC = 8'hDD;

    typedef enum logic [1:0] { IDLE, ESC_END, ESC_ESC } slip_state_e;

    slip_state_e state;

    // Back-pressure: high whenever finishing an escape sequence
    assign o_busy = (state != IDLE);

    always_comb begin
        o_valid = 1'b0;
        o_data  = 8'h00;

        case (state)
            IDLE: begin
                if (i_frame_end) begin
                    o_data  = SLIP_END;
                    o_valid = 1'b1;
                end else if (i_valid) begin
                    if (i_data == SLIP_END || i_data == SLIP_ESC) begin
                        // First byte of escape sequence
                        o_data  = SLIP_ESC;
                        o_valid = 1'b1;
                    end else begin
                        o_data  = i_data;
                        o_valid = 1'b1;
                    end
                end
            end
            ESC_END: begin
                o_data  = SLIP_ESC_END;
                o_valid = 1'b1;
            end
            ESC_ESC: begin
                o_data  = SLIP_ESC_ESC;
                o_valid = 1'b1;
            end
            default: ;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state <= IDLE;
        end else begin
            case (state)
                IDLE: begin
                    if (i_valid) begin
                        if (i_data == SLIP_END)
                            state <= ESC_END;
                        else if (i_data == SLIP_ESC)
                            state <= ESC_ESC;
                    end
                end
                ESC_END: state <= IDLE;
                ESC_ESC: state <= IDLE;
                default: state <= IDLE;
            endcase
        end
    end

endmodule
