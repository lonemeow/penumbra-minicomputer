// Penumbra Ethernet station address from a 24AAxxxE48 EEPROM
//
// Reads the EUI-48 that a Microchip 24AA02E48 or 24AA025E48 holds and
// presents it as the Ethernet device's factory address
// (doc/system/devices/ethernet.md, "FACTORY_LO / FACTORY_HI"). The part
// keeps the address in its write-protected half: six bytes from word
// address 0xFA, in the order they go on the wire (datasheet,
// "Pre-programmed EUI-48 Node Address").
//
// It drives an i2c_master's command port, a byte to a command, through
// one transaction: the part's random read, which sets its address
// pointer, carried on as a sequential read of the six bytes (datasheet,
// "Read Operation").
//
// The read begins at reset and waits, as any command does, for the
// master to take it. Where the bus carries other devices the master is
// an i2c_shared_master, and nothing here is different for that.
//
// The read fails if the part does not acknowledge a byte written to it,
// or if the master reports a bus error. A failed read leaves o_valid
// clear for good, and the interface has a driver choose an address then.
// Nothing is tried again: the part refuses a read only while it is
// writing, and nothing here writes it.

module eth_addr_eeprom #(
    // The part's address on the bus: 1010, then its three chip-select
    // pins as the board straps them. It has no default.
    parameter logic [6:0] DEV_ADDR
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── To the I2C master ────────────────────────────────────
    output logic        o_cmd_valid,
    input  logic        i_cmd_ready,
    output logic        o_cmd_start,
    output logic        o_cmd_stop,
    output logic        o_cmd_read,
    output logic [7:0]  o_cmd_wdata,
    output logic        o_cmd_nack,
    input  logic        i_rsp_valid,
    input  logic [7:0]  i_rsp_rdata,
    input  logic        i_rsp_nack,
    input  logic        i_rsp_error,

    // ── The station address ──────────────────────────────────
    output logic [47:0] o_addr,         // its first byte in [7:0]
    output logic        o_valid         // held: rises once the address is read, and stays
);

    localparam logic [7:0] WORD_ADDR  = 8'hFA;  // where the part keeps its EUI-48
    localparam int         ADDR_BYTES = 6;

    // The part's control byte: its address, then the direction.
    localparam logic [7:0] CTRL_WRITE = {DEV_ADDR, 1'b0};
    localparam logic [7:0] CTRL_READ  = {DEV_ADDR, 1'b1};

    // One of the master's commands: a byte written or read, and what
    // goes around it on the bus.
    typedef struct packed {
        logic       start;      // a START, or a repeated one, ahead of the byte
        logic       stop;       // a STOP behind it
        logic       read;       // the byte is read, not written
        logic       nack;       // a byte read is answered with NACK
        logic [7:0] wdata;      // a byte written
    } cmd_t;

    // A command is written as its parts, ORed: each of these sets one
    // field and leaves the rest clear.
    localparam cmd_t START = '{start: 1'b1, default: '0};
    localparam cmd_t STOP  = '{stop:  1'b1, default: '0};
    localparam cmd_t READ  = '{read:  1'b1, default: '0};
    localparam cmd_t NACK  = '{nack:  1'b1, default: '0};

    function automatic cmd_t write_byte(input logic [7:0] data);
        write_byte = '{wdata: data, default: '0};
    endfunction

    // The transaction, a command to a step: three bytes written to
    // address the part and point it at the EUI-48, then the address's
    // bytes read.
    localparam int STEPS  = 3 + ADDR_BYTES;
    localparam int STEP_W = $clog2(STEPS);

    typedef logic [STEP_W-1:0] step_t;
    localparam step_t LAST_STEP = step_t'(STEPS - 1);

    function automatic cmd_t step_cmd(input step_t step);
        case (step)
            0:         step_cmd = START | write_byte(CTRL_WRITE);
            1:         step_cmd =         write_byte(WORD_ADDR);
            2:         step_cmd = START | write_byte(CTRL_READ);

            // The part sends for as long as its bytes are acknowledged.
            LAST_STEP: step_cmd = READ | NACK | STOP;
            default:   step_cmd = READ;
        endcase
    endfunction

    typedef enum logic [1:0] {
        S_SEND,         // the step's command offered to the master
        S_ANSWER,       // waiting for the master's response to it
        S_DONE          // nothing more to ask, the address read or not
    } state_t;

    state_t      state_q;
    step_t       step_q;
    logic [47:0] addr_q;
    logic        valid_q;

    cmd_t cmd;
    assign cmd = step_cmd(step_q);

    assign o_cmd_valid = (state_q == S_SEND);
    assign o_cmd_start = cmd.start;
    assign o_cmd_stop  = cmd.stop;
    assign o_cmd_read  = cmd.read;
    assign o_cmd_nack  = cmd.nack;
    assign o_cmd_wdata = cmd.wdata;

    assign o_addr  = addr_q;
    assign o_valid = valid_q;

    // A byte written that the part did not acknowledge, or a bus error.
    // The master says nothing of acknowledgement for a byte read.
    logic failed;
    assign failed = i_rsp_error || (!cmd.read && i_rsp_nack);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q <= S_SEND;
            step_q  <= '0;
            valid_q <= 1'b0;
        end else begin
            unique case (state_q)
                S_SEND: begin
                    if (i_cmd_ready)
                        state_q <= S_ANSWER;
                end

                S_ANSWER: begin
                    if (i_rsp_valid) begin
                        // The first byte read ends in [7:0].
                        if (cmd.read)
                            addr_q <= {i_rsp_rdata, addr_q[47:8]};

                        if (failed) begin
                            state_q <= S_DONE;
                        end else if (step_q == LAST_STEP) begin
                            valid_q <= 1'b1;
                            state_q <= S_DONE;
                        end else begin
                            step_q  <= step_q + step_t'(1);
                            state_q <= S_SEND;
                        end
                    end
                end

                S_DONE: ;
            endcase
        end
    end

endmodule
