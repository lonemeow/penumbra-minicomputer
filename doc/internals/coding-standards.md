# Penumbra Hardware - Coding Standards

This document defines the RTL coding standards for the Penumbra project. All hardware modules written in SystemVerilog must adhere to these conventions to ensure consistency, maintainability, and compatibility with the toolchain (Verilator, Yosys).

## 1. Language and Style

- **Language:** SystemVerilog 2012.
- **Keywords:** Use `logic` instead of `reg` or `wire` for all signals except where `tri` or similar net types are strictly required (e.g., bi-directional buses).
- **Indentation:** 4 spaces. No tabs.
- **Module Instances:** Use named port connections: `.port_name(signal_name)`. Never use positional connections.
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

## 3. Combinational and Sequential Logic

- **Combinational:** Use `always_comb` blocks. Do not use `always @*`.
- **Sequential:** Use `always_ff @(posedge i_clk)` blocks.
- **Assignments:**
    - Use non-blocking assignments (`<=`) in `always_ff` blocks.
    - Use blocking assignments (`=`) in `always_comb` blocks and for `assign` statements.
- **Latches:** Implicit latches are strictly forbidden. Every `always_comb` block must have all paths assigned, and every `if` should have an `else`. Use default assignments at the top of the block if necessary.

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
- **Lint Offsets:** If a specific lint warning must be suppressed, use the Verilator inline comments:
  ```systemverilog
  /* verilator lint_off UNUSEDSIGNAL */
  input logic i_unused;
  /* verilator lint_on UNUSEDSIGNAL */
  ```

## 6. Documentation and Comments

- Use `//` for single-line comments.
- Use `/* ... */` for block comments.
- Every module should have a header comment explaining its purpose, inputs, outputs, and any specific timing or protocol requirements.
- Large blocks of code should be separated by decorative headers:
  ```systemverilog
  // ── Sub-section Title ───────────────────────────────────────
  ```
