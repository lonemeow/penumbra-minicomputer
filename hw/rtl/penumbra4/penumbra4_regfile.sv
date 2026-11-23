// penumbra4_regfile — the physical registers: four read ports, two write
// ports.
//
// Reads are combinational; a write lands at the clock edge, so a read sees
// it from the next cycle. The two writes of a cycle never name one
// register, as commit drops the older of two writes to one register.
//
// Flops, the smallest form; the full form is XOR banks over distributed RAM
// behind the same ports (doc/internals/penumbra4/regfile.md).

module penumbra4_regfile
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic                   i_clk,
    input  logic                   i_rst,

    input  phys_reg_t  [3:0]       i_read_phys,
    output word_t      [3:0]       o_read_value,

    input  reg_write_t [1:0]       i_write
);

    localparam int ENTRIES = 1 << $bits(phys_reg_t);

    (* ram_style = "registers" *) word_t entries_q [ENTRIES];

    always_ff @(posedge i_clk) begin
        for (int p = 0; p < 2; p++) begin
            if (i_write[p].en) begin
                entries_q[i_write[p].phys] <= i_write[p].value;
            end
        end
    end

    always_comb begin
        for (int p = 0; p < 4; p++) begin
            o_read_value[p] = entries_q[i_read_phys[p]];
        end
    end

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst) begin
            assert (!(i_write[0].en && i_write[1].en && i_write[0].phys == i_write[1].phys))
                else $error("penumbra4_regfile: two writes to one register");
        end
    end

endmodule
