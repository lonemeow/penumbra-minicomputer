// SLIP receive decoder — strips RFC 1055 framing from a byte stream
//
// Accepts raw bytes from UART RX (via i_valid/i_data), decodes SLIP
// escape sequences, and outputs clean payload bytes (o_valid/o_data).
// Pulses o_frame_end when a complete frame delimiter is received.
//
// SLIP special bytes:
//   END  = 0xC0 — frame delimiter
//   ESC  = 0xDB — escape prefix
//   0xDC after ESC — literal 0xC0
//   0xDD after ESC — literal 0xDB

module slip_rx
    import penumbra_pkg::*;
(
    input  logic       i_clk,
    input  logic       i_rst,

    // Raw byte input (from UART RX)
    input  logic       i_valid,     // byte available this cycle
    input  logic [7:0] i_data,      // raw byte

    // Decoded payload output
    output logic       o_valid,     // decoded payload byte available
    output logic [7:0] o_data,      // decoded payload byte
    output logic       o_frame_end  // pulse: complete frame received
);

    // SLIP special byte constants
    localparam logic [7:0] SLIP_END = 8'hC0;
    localparam logic [7:0] SLIP_ESC = 8'hDB;
    localparam logic [7:0] SLIP_ESC_END = 8'hDC;  // ESC + this = literal END
    localparam logic [7:0] SLIP_ESC_ESC = 8'hDD;  // ESC + this = literal ESC

    typedef enum logic { NORMAL, ESCAPE } slip_state_e;

    slip_state_e state;

    always_comb begin
        // Defaults — no output unless i_valid and a real byte
        o_valid     = 1'b0;
        o_data      = 8'h00;
        o_frame_end = 1'b0;

        if (i_valid) begin
            case (state)
                NORMAL: begin
                    o_data      = i_data;
                    o_valid     = (i_data != SLIP_END && i_data != SLIP_ESC);
                    o_frame_end = i_data == SLIP_END;
                end
                ESCAPE: begin
                    case (i_data)
                        SLIP_ESC_END: begin
                            o_data  = SLIP_END;
                            o_valid = 1'b1;
                        end
                        SLIP_ESC_ESC: begin
                            o_data  = SLIP_ESC;
                            o_valid = 1'b1;
                        end
                        default: begin
                            // Invalid escape — pass through raw byte
                            o_data  = i_data;
                            o_valid = 1'b1;
                        end
                    endcase
                end
            endcase
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state <= NORMAL;
        end else if (i_valid) begin
            case (state)
                NORMAL: begin
                    if (i_data == SLIP_ESC)
                        state <= ESCAPE;
                end
                ESCAPE: state <= NORMAL;
            endcase
        end
    end

endmodule
