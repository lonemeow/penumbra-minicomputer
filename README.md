# Penumbra Minicomputer

A hobby project to design and build a vintage-style minicomputer implemented in FPGA, inspired by classic machines like the Data General Eclipse/MV series and the DEC VAX.

## Project Goals

- Design a complete 32-bit minicomputer from the ground up, including CPU, MMU, DMA controllers, I/O buses, and peripherals
- Implement a clean load-store, RISC-like ISA
- Full MMU with virtual memory and memory protection
- Learn hardware design across the entire machine, not just the CPU core
- Eventually use lessons learned to build a second design from discrete 74xx logic on custom PCBs

## Architecture Overview

- **Word size:** 32-bit
- **ISA style:** Load-store, RISC-like
- **Memory management:** Full MMU with virtual memory, page-based protection
- **Design philosophy:** Inspired by 1970s/80s minicomputers (DG Eclipse, DEC VAX) but with a clean, modern RISC sensibility

## Hardware Platform

- **FPGA board:** [Radiona ULX3S](https://radiona.org/ulx3s/) (Lattice ECP5 FPGA)
- **Toolchain:** Open-source (Yosys, nextpnr, Project Trellis)

## Repository Structure

```
hw/               - Hardware design
  rtl/            - Synthesizable RTL (SystemVerilog)
    core/         - CPU core (ALU, register file, decode, control)
    mmu/          - Memory management unit
    bus/          - System bus and arbitration
    io/           - I/O controllers and peripherals
    soc/          - Top-level SoC integration
  sim/            - Simulation testbenches and test programs
  microcode/      - Microcode source (assembled into ROM)
  rom/            - Boot ROM firmware
  tools/          - Microcode assembler (uasm.py)
  constraints/    - FPGA pin constraints and timing for ULX3S
sw/               - Software tools
  tools/          - ISA assembler (pasm.py)
doc/              - Architecture documentation
  isa/            - ISA specification
  mmu/            - MMU and memory map documentation
  bus/            - Bus protocol documentation
  toolchain/      - LLVM backend strategy
```

## Status

CPU runs real programs in simulation with full CPU, MMU, split I/D cache, and memory-mapped UART. Boot ROM monitor operational with interactive terminal I/O.

## License

This project is open source. See [LICENSE](LICENSE) for details.
