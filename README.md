# Penumbra Minicomputer

A 32-bit RISC minicomputer designed from scratch and implemented on FPGA.
Inspired by classic machines like the Data General Eclipse and DEC VAX, but
with a clean load-store ISA. The eventual goal is to run NetBSD and later
build the design from discrete 74xx logic chips.

---

## ⚡ Quick Start

```sh
# 1. Clone with submodules
git clone --recurse-submodules https://github.com/<user>/penumbra-minicomputer.git
cd penumbra-minicomputer

# 2. Run hardware tests (no LLVM required)
make test

# 3. Build boot ROM and run interactive simulation (requires LLVM, see DEVELOP.md)
make simulate
```

---

## 🏗️ Architecture Summary

Penumbra is a modern RISC implementation with a "classic" aesthetic.

- **Word size:** 32-bit, little-endian.
- **ISA:** 4 instruction formats (R/L/M/B), 16 GPRs (R0=zero, R14=SP, R13=LR, R15=PC).
- **Execution:** 3-bus datapath, 51-bit horizontal microcode, 256-entry ROM.
- **MMU:** Software-managed 64-entry 2-way SA TLB + 4-entry FA pinned TLB.
- **Memory:** Split I/D PIPT caches, write-through D-cache.
- **Bus:** Asynchronous Penumbra Bus with 4-phase handshake and autoconfig.

Detailed specifications are available in the **[Documentation Index](doc/README.md)**.

---

## 📈 Status

The system is fully functional in cycle-accurate and instruction-level simulation.

- **Hardware:** All RTL modules (CPU, MMU, Cache, Bus, UART, SPI) implemented and verified.
- **Toolchain:** Custom LLVM backend (clang/lld) fully operational.
- **OS:** NetBSD 10.1 port in progress; kernel compiles and reaches link stage.
- **Firmware:** C boot ROM with FAT32 support, PIE ELF loading, and monitor commands.

---

## 📁 Repository Layout

- `hw/` — RTL design (SystemVerilog), testbenches, microcode, and boot ROM.
- `sw/` — Instruction set simulator (ISS) and assembler tools.
- `llvm/` — Penumbra backend for the LLVM compiler infrastructure.
- `doc/` — Comprehensive architecture and system documentation.
- `netbsd/` — NetBSD kernel and userland source tree.
- `benchmark/` — Dhrystone and other bare-metal performance tests.

---

## 🛠️ Development

For instructions on building the LLVM toolchain, compiling the NetBSD kernel, and running the full test suite, see **[DEVELOP.md](DEVELOP.md)**.

## 📜 License

This project is open source, licensed under the **BSD 2-Clause License**. See [LICENSE](LICENSE) for details and third-party component licenses.
