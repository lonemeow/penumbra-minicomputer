# Penumbra Minicomputer

A 32-bit RISC minicomputer designed from scratch and implemented on FPGA.
Inspired by classic machines like the Data General Eclipse and DEC VAX, but
with a clean load-store ISA. It boots NetBSD 10.1 to a full userland on real
hardware; the longer-term goal is to rebuild the design from discrete 74xx
logic chips.

---

## 💡 Why Penumbra?

Penumbra is born from a desire to move beyond theory. While reading books on computer architecture provides a foundation, it is no substitute for the hands-on experience of making design choices and living with their consequences.

### Inspiration
The primary spark for this project was Bill Buzbee's [Magic-1](https://homebrewcpu.com), a homebrew CPU built from wire-wrapped TTL chips. Discovering it over a decade ago transformed a long-standing interest in ISA design into a concrete goal: to build a complete, functional system from the ground up.

### Design Philosophy
- **Modular for Experimentation:** The architecture is intentionally modular to facilitate learning. It is designed to allow swapping and comparing different components—such as varying cache geometries or TLB designs—to empirically observe their trade-offs.
- **Software-Driven Discovery:** System register interfaces are designed for autodetection. This allows software to dynamically adapt to hardware variations, supporting the project's role as an experimental platform for architectural exploration.
- **Vertical Integration:** By building everything from the RTL and microcode up through the LLVM backend and a NetBSD port, the project provides a comprehensive view of the entire hardware-software contract.

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

Penumbra is a modern RISC implementation with a "classic" aesthetic. The same
ISA is realized by two interchangeable CPU cores:

- **Penumbra/1** — a single-cycle microcoded core (3-bus datapath, 51-bit
  horizontal microcode, 256-entry control ROM). Deliberately simple enough to
  rebuild in 74xx discrete logic.
- **Penumbra/2** — a 6-stage hardwired pipeline (IF1 / IF2 / ID / EX / MEM /
  WB) with scoreboard-based hazard tracking, NZCV flag forwarding, and branch
  resolution in EX. No microcode.

Shared across both cores:

- **Word size:** 32-bit, little-endian.
- **ISA:** 4 instruction formats (R/L/M/B), 16 GPRs (R0=zero, R14=SP, R13=LR, R15=PC).
- **MMU:** Software-managed 64-entry 2-way SA TLB + 4-entry FA pinned TLB.
- **Caches:** VIPT split I/D L1, write-through, backed by a 64 KiB unified 4-way L2 with write-invalidate-on-hit. The L1 geometry is per-core — gen1 is 1 KiB direct-mapped, gen2 is 4 KiB 4-way (BRAM-backed).
- **Memory:** 32 MB SDRAM on the ULX3S target, reached through an async CDC bridge from the CPU clock to the SDRAM clock.
- **Bus:** Asynchronous Penumbra Bus with 4-phase handshake and Zorro-style autoconfig.

Detailed specifications are available in the **[Documentation Index](doc/README.md)**.

---

## 📈 Status

The system runs on real hardware (Radiona ULX3S, Lattice ECP5-85F) as well as in cycle-accurate Verilator simulation and a fast instruction-level simulator.

- **CPU cores:** Two complete, interchangeable implementations of the ISA. Penumbra/2 (6-stage pipelined) is the default and passes the full ISA conformance suite (72/72); Penumbra/1 (microcoded) remains the discrete-logic reference. Both synthesize to the ULX3S, close timing at the 25 MHz CPU target on the ECP5-85F, and boot NetBSD to userland. Select the gen1 core with `CORE=penumbra1` on `make` targets.
- **Hardware:** Full CPU + MMU + split L1 caches + unified L2 cache + async Penumbra Bus with autoconfig + UART + SPI/SD + SDRAM controller + a from-scratch USB 1.1 host controller (SIE, MAC, and PHY tiers), all running on the ULX3S board — the USB port drives real commercial devices: flash drives, keyboards, hubs, and Ethernet adapters.
- **Toolchain:** Custom LLVM backend (clang/lld/llvm-mc) end-to-end; PIE/GOT/PLT, TLS, soft-float, C++ EH with DWARF unwinding.
- **OS:** NetBSD 10.1 boots to userland on the ULX3S hardware (on either CPU core), mounting an FFS root from SD card (read + write). Dynamic linking, fork/exec, pipes, signals, and TLS all functional; the in-tree `pbench` microbenchmark suite exercises kernel syscalls and libc hot paths dynamically linked against `libc.so` on real hardware. The MI USB stack runs over the custom host controller: mass-storage disks attach as `sd(4)` and mount read/write, HID keyboards deliver reports through `uhid(4)`, full-speed devices work behind cascaded hubs, and hot-plug attach/detach is handled throughout. Networking runs over a USB Ethernet adapter (`ure(4)`): the machine holds an IP address, answers and issues pings, and connects out over `ssh`.
- **Firmware:** C boot ROM with FAT32 support, PIE ELF loading, bus autoconfig, and an interactive monitor.

---

## 📁 Repository Layout

- `hw/` — RTL design (SystemVerilog), testbenches, microcode, and boot ROM.
- `sw/` — Instruction set simulator (ISS) and assembler tools.
- `llvm/` — Penumbra backend for the LLVM compiler infrastructure.
- `doc/` — Comprehensive architecture and system documentation.
- `netbsd/` — NetBSD kernel and userland source tree.
- `benchmark/` — Bare-metal benchmarks (Dhrystone, membench, memtest) and the NetBSD-hosted `pbench` microbenchmark suite.

---

## 🛠️ Development

For instructions on building the LLVM toolchain, compiling the NetBSD kernel, and running the full test suite, see **[DEVELOP.md](DEVELOP.md)**.

## 📜 License

This project is open source, licensed under the **BSD 2-Clause License**. See [LICENSE](LICENSE) for details and third-party component licenses.
