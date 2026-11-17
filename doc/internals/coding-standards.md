# Penumbra Hardware - Coding Standards

> **Applies to:** all generations · shared hardware.

This document defines the RTL coding standards for the Penumbra project. All hardware modules written in SystemVerilog must adhere to these conventions to ensure consistency, maintainability, and compatibility with the toolchain (Verilator, Yosys).

## 1. Language and Style

- **Language:** SystemVerilog 2012.
- **Keywords:** Use `logic` instead of `reg` or `wire` for all signals except where `tri` or similar net types are strictly required (e.g., bi-directional buses).
- **Indentation:** 4 spaces. No tabs.
- **Module Instances:** Use named port connections: `.port_name(signal_name)`. Never use positional connections. Place each parameter override and each port connection on its **own line**, even when short. A new port or parameter is then a one-line addition instead of a reflowed block, which keeps git diffs minimal and reviewable.
- **Module Declarations:** One module per file. Filename must match the module name (e.g., `alu.sv` contains `module alu`).

## 2. Naming Conventions

### Ports
Prefix all module ports to distinguish them from internal signals:
- `i_` for inputs (e.g., `i_data`, `i_en`)
- `o_` for outputs (e.g., `o_result`, `o_busy`)
- `io_` for bi-directional signals (rarely used)

### Global Signals
- **Clock:** `i_clk`. Synchronous, rising-edge triggered.
- **Reset:** `i_rst`. Synchronous, active-high. Resets state to power-on defaults.

### Signal Names
- Use `snake_case` for all signals, ports, and modules.
- Parameters and localparams should be `SCREAMING_SNAKE_CASE`.
- Active-low signals (if used) should end in `_n` (e.g., `o_irq_n`).
- **Registered vs combinational must be visible in the name.** A signal that
  holds registered state (assigned in an `always_ff`) ends in `_q`; the
  combinational next-state value that feeds it ends in `_d`
  (`count_q <= count_d`). A purely combinational signal that doesn't back a
  specific flop takes a plain descriptive name. This makes "is this a flop
  output or a wire?" answerable at every use site, without tracing the
  assignment. Ports keep their `i_`/`o_` role; a registered output is driven
  from a `_q` signal.
- **Name by semantic meaning, not mechanism or incidental use.** Prefer the
  name that states what a signal *means*: `i_first_cycle` over `i_fresh`,
  `o_line_valid` over `o_v`. A longer, explicit name that conveys intent is
  worth the characters.

## 3. Combinational and Sequential Logic

- **Combinational:** Use `always_comb` blocks. Do not use `always @*`.
- **Sequential:** Use `always_ff @(posedge i_clk)` blocks.
- **Assignments:**
    - Use non-blocking assignments (`<=`) in `always_ff` blocks.
    - Use blocking assignments (`=`) in `always_comb` blocks and for `assign` statements.
- **Latches:** Implicit latches are strictly forbidden. Every `always_comb` block must have all paths assigned, and every `if` should have an `else`. Use default assignments at the top of the block if necessary.

### Rules for new modules

A module written from scratch follows four more rules. A module that
already exists keeps its own style when it changes, until it is
rewritten whole, and one that fits a new design unchanged, such as a
device, is used as it is.

- **A value's type says what it is.** A physical address is a
  `paddr_t`, a register number a `reg_index_t`, an instruction an
  `insn_t`, never a bare `logic [31:0]` or `logic [3:0]`. Two values
  of one width that mean different things take different types, so a
  port list reads as what crosses it. The names document rather than
  enforce: SystemVerilog converts between vector types of one width
  without a word, so a mixed-up assignment still compiles. A type
  modules share lives in a package; one a single module uses is
  declared in that module, sized from its parameters. Type names end
  in `_t`, enum type names in `_e`. A bare width stays for a value
  whose bits are all it is, such as a counter inside one block.
- **A value made of fields is a packed struct.** Code names the field
  (`pte.ppn`) rather than slicing bits out of it (`pte[31:12]`), and the
  struct's declaration is the one place the layout is written. A layout
  an outside contract fixes, such as the instruction encoding, a PTE or
  a sysreg, is declared once in the package, as that contract gives it.
  A slice stays where the bits are arithmetic rather than fields: a
  sign bit, a carry out, the low bits of a sum.
- **A register block holds only the register:** an `always_ff` block
  contains the reset value, an optional enable and the assignment from
  `_d` to `_q`. The logic that computes `_d` goes in `always_comb` or
  `assign`. Every flop and its reset value are then in one place, and
  what the module computes is in another.
- **A state machine's states are an enum,** with one combinational
  block for the next state and the outputs, and one register block.

```systemverilog
typedef enum logic [1:0] {
    IDLE,
    RUNNING,
    DRAINING
} state_e;

state_e state_q, state_d;

always_comb begin
    state_d = state_q;
    unique case (state_q)
        IDLE:     if (i_start) state_d = RUNNING;
        RUNNING:  if (i_stop)  state_d = DRAINING;
        DRAINING: if (i_empty) state_d = IDLE;
    endcase
end

always_ff @(posedge i_clk) begin
    if (i_rst) begin
        state_q <= IDLE;
    end else begin
        state_q <= state_d;
    end
end

assign o_busy = (state_q != IDLE);
```

## 4. Packages and Constants

- Common constants (opcodes, register addresses, etc.) live in `hw/rtl/common/penumbra_pkg.sv`.
- **Imports:** Import the package *inside* the module scope to avoid Verilator $unit scope warnings:
  ```systemverilog
  module my_module (
      // ports
  );
      import penumbra_pkg::*;
      // module body
  endmodule
  ```

## 5. Toolchain Compatibility

- **Verilator:** All modules must pass Verilator linting (`-Wall`).
- **Yosys:** Avoid complex SystemVerilog features that Yosys does not yet support (e.g., interfaces with complex modports, some types of unpacked arrays). Stick to "Synthesizable SystemVerilog."
- **Build-time checks use `build_check`.** A check on configuration
  (a parameter combination the logic cannot honor, a PLL with no
  legal divider set) instantiates `hw/rtl/common/build_check.sv`,
  never an `initial assert` or a generate-block `$error`/`$fatal`.
  sv2v, which feeds every synthesis flow, deletes the assertion and
  turns the severity task into a message synthesis prints and then
  carries on from, so an impossible configuration ships as a
  bitstream. `build_check` stops elaboration in Verilator, yosys and
  Vivado alike; its header documents the interface. Assertions about
  behavior during simulation (`assert property`, asserts in `always`
  blocks) stay assertions: they are simulation-only by intent. Since
  sv2v deletes them, asserts alone in a procedural `for` leave a loop
  with an empty body, which Vivado refuses to read; loop over them
  with a generate `for` instead.
- **Memories map the same way under yosys and Vivado.** Every memory
  (an unpacked array indexed by a variable) declares its storage with
  an attribute both tools read: `(* ram_style = "block" *)`,
  `"distributed"` (LUT RAM) or `"registers"`, and
  `(* rom_style = "block" *)` for a ROM. yosys fails a build whose
  attribute it cannot honor; Vivado only warns and falls back to other
  storage, so the xc7 signoff gates (`hw/tools/vivado/gates.tcl`) fail
  on those warnings. The declaration only helps if the code is written
  in a form both tools map:
    - **Block RAM:** one memory variable per independently addressed
      array, such as one per cache way, never a `[ways][sets]` array.
      At most two ports, and each write port is a single write
      statement: when several sources share it, their address, data
      and byte lanes are chosen before the write. The write sits in an
      `always_ff` with no reset (resets belong to control state, not to
      the array), and the read is registered. yosys merges several
      write sites or packs a 2-D array into one block RAM by itself;
      Vivado builds either from flip-flops or LUT RAM instead. A write
      wider than the read (a line written, a word read) maps in Vivado
      only when the whole wide row is written under one enable, as in
      UG901's asymmetric RAM: a write that can also update one narrow
      entry of the row is built from registers (`penumbra3_l1_store`
      keeps word columns instead).
    - **Read-during-write:** where a read could meet a write at one
      address, yosys emulates the RTL's result with a delayed write and
      a mux after the block RAM, and only exclusive enables rule that
      out. Gate one enable by the other where the exclusion is by
      enable. Where it is by address, mark the memory `no_rw_check` and
      assert beside it, on the same enables and addresses, that no
      entry is read and written in one cycle (`penumbra3_l1_store`);
      never for an exclusion another module keeps. Where the two may
      meet but the read then takes the written data from a register
      beside the block RAM, never its output, mark the memory
      `no_rw_check` too and assert that such a read gives the written
      data (`penumbra3_direction_table`).
    - **LUT RAM:** one write port; the read may be combinational. A
      second independent read port gets its own copy of the memory, as
      the regfile's `regs_a`/`regs_b` do.
    - **Registers:** arrays that need a reset, or whose every entry is
      read at once (PLRU state, valid vectors, the pinned TLB).

  ```systemverilog
  (* ram_style = "block" *) logic [31:0] mem [DEPTH];

  always_ff @(posedge i_clk) begin
      if (rd_en)
          rdata_q <= mem[rd_addr];
      if (wr_en) begin
          if (wr_be[0]) mem[wr_addr][ 7: 0] <= wr_data[ 7: 0];
          if (wr_be[1]) mem[wr_addr][15: 8] <= wr_data[15: 8];
          if (wr_be[2]) mem[wr_addr][23:16] <= wr_data[23:16];
          if (wr_be[3]) mem[wr_addr][31:24] <= wr_data[31:24];
      end
  end
  ```
- **Lint Offsets:** If a specific lint warning must be suppressed, use the Verilator inline comments:
  ```systemverilog
  /* verilator lint_off UNUSEDSIGNAL */
  input logic i_unused;
  /* verilator lint_on UNUSEDSIGNAL */
  ```

## 6. Documentation and Comments

- Use `//` for single-line comments.
- Use `/* ... */` for block comments.
- **Comment the *why*, not the *what*.** The code already states what it
  does; a comment restating it is noise. Reserve comments for intent,
  rationale, and non-obvious consequences — what the code cannot say itself.
  A genuinely non-trivial mechanism deserves explanation; a plain assignment
  does not.
- **Comments are non-temporal.** Describe the code as it is, not how it got
  there — no "changed from…", "now…", or what a signal "used to" do. That
  history lives in git and does not help a later reader.
- Every module has a header comment stating its purpose and any non-obvious
  timing or protocol contract. Don't enumerate every port — the port list
  and good names already cover that; call out only the ports with a subtle
  contract.
- Large blocks of code should be separated by decorative headers:
  ```systemverilog
  // ── Sub-section Title ───────────────────────────────────────
  ```
