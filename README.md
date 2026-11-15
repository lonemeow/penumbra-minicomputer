# Penumbra Minicomputer

A 32-bit RISC minicomputer designed from scratch and implemented on FPGA.
Inspired by classic machines like the Data General Eclipse and DEC VAX, but
with a clean load-store ISA. It boots NetBSD 10.1 to a full userland on real
hardware; the longer-term goal is to rebuild the design from discrete 74xx
logic chips.

![penmon on the HDMI console](doc/images/penmon.png)

*`penmon`, the machine's live system monitor, on the HDMI console.*

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

## 🏗️ Architecture

One ISA, several cores.

- **ISA:** 32-bit load-store RISC, little-endian. 16 GPRs, 4
  instruction formats.
- **Arithmetic:** MUL/DIV in hardware. Floating point in software.
- **MMU:** software-managed TLB with ASIDs. 64-entry 2-way main TLB
  plus 4 pinned entries.
- **Caches:** split VIPT L1, write-through. Shared 64 KB 4-way L2.
- **Bus:** asynchronous 4-phase Penumbra Bus, Zorro-style
  autoconfig.
- **Memory:** 32 MB SDRAM on its own clock domain.

Cores:

- **Penumbra/1:** microcoded, 3-bus datapath, 25 MHz. The 74xx
  discrete-logic reference. 1 KB direct-mapped L1s.
- **Penumbra/2:** 6-stage hardwired pipeline, 37.5 MHz. 4 KB 2-way
  L1s.
- **Penumbra/2.5:** Penumbra/2 plus forwarding and a BTB, 37.5 MHz.
  The default core, and the fastest.
- **Penumbra/3:** in-order pipeline redo, aimed at 50 MHz. Under
  construction.

Specs live in the **[Documentation Index](doc/README.md)**.

---

## 📈 Status

Runs on a Radiona ULX3S (Lattice ECP5-85F). Also runs in a fast
instruction-level simulator and in cycle-accurate Verilator.

**Hardware**, all built from scratch:

- CPU, MMU, caches, SDRAM controller.
- UART console.
- SD card over SPI.
- USB 1.1 host controller, down to the PHY. Drives real flash
  drives, keyboards, hubs and Ethernet adapters.
- HDMI display: text console plus a framebuffer.
- 4-channel audio mixer on the 3.5 mm jack.
- Performance counters: stalls, cache hits and misses.

**Software:**

- LLVM backend: clang, lld, llvm-mc. PIE, TLS, C++ exceptions.
- Boot ROM in C: FAT32, ELF loader, monitor.
- NetBSD 10.1 with its full userland, on Penumbra/1 and /2:
  - Root filesystem on SD card.
  - USB disks and keyboards, with hot-plug.
  - Networking over USB Ethernet: ping, ssh.
  - Local console on HDMI and a USB keyboard.
  - Audio through `audio(4)` and a multi-voice `/dev/pmusic`.
  - Dynamic linking, fork/exec, signals, core dumps.
- Runs Doom, graphics demos and a ProTracker MOD player.
- SD images: single-user, multiuser, and an unattended exhibit
  kiosk.

---

## 📁 Repository Layout

- `hw/` — RTL design (SystemVerilog), testbenches, microcode, and boot ROM.
- `sw/` — Instruction set simulator (ISS), assembler tools, and
  programs for NetBSD: demos, Doom, the MOD player, the kiosk
  launcher.
- `llvm/` — Penumbra backend for the LLVM compiler infrastructure.
- `doc/` — Comprehensive architecture and system documentation.
- `netbsd/` — NetBSD kernel and userland source tree.
- `benchmark/` — Bare-metal benchmarks (Dhrystone, membench, memspeed, memtest) and the NetBSD-hosted `pbench` microbenchmark suite.

---

## 🛠️ Development

For instructions on building the LLVM toolchain, compiling the NetBSD kernel, and running the full test suite, see **[DEVELOP.md](DEVELOP.md)**.

## 📜 License

This project is open source, licensed under the **BSD 2-Clause License**. See [LICENSE](LICENSE) for details and third-party component licenses.
