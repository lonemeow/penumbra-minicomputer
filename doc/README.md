# Penumbra Documentation

Welcome to the Penumbra Minicomputer documentation. This directory is organized by audience and technical domain to ensure information is relevant and easy to find.

## 📚 [System Reference](./system)
**Target:** Compiler writers, OS porters, and driver developers.

This section defines the programmer-visible behavior of the Penumbra architecture and its standard peripherals.

### ISA & Software Environment
*   **[Architecture Overview](./system/architecture.md)** — Design philosophy, registers, and memory model.
*   **[Instruction Set Guide](./system/instruction-set.md)** — Complete programmer's reference for all instructions.
*   **[ABI Specification](./system/abi.md)** — Data models, calling conventions, and ELF details.
*   **[Toolchain Strategy](./system/toolchain.md)** — LLVM backend and compiler considerations.

### System Services
*   **[MMU & TLB](./system/mmu.md)** — Software-managed translation, protection, and dirty tracking.
*   **[System Registers](./system/sysregs.md)** — Device-mapped control registers (Device Map).
*   **[Bus & Address Map](./system/bus.md)** — Physical memory layout and the autoconfig software flow.
*   **[Boot Protocol](./system/boot-protocol.md)** — The three-stage ROM → Loader → Kernel handoff.

### Device Drivers
*   **[SPI / SD Card](./system/devices/spi.md)** — Register map and driver flow for storage.
*   **[ESP32 WiFi NIC](./system/devices/esp32-nic.md)** — Ethernet-over-SLIP network adapter interface.

### Operating System
*   **[NetBSD Port Status](./system/netbsd/porting-status.md)** — Current status of the NetBSD 10 port.
*   **[NetBSD Design Notes](./system/netbsd/design-notes.md)** — Architectural decisions for the Penumbra port.

---

## 🛠️ [Hardware Design](./hardware)
**Target:** Board designers and external device implementers.

Specifications for interfacing with the Penumbra async external bus.

*   **[Bus Protocol](./hardware/bus-protocol.md)** — 4-phase handshake and signal timing.
*   **[Autoconfig Hardware](./hardware/autoconfig-hardware.md)** — Electrical implementation of the `cfg` daisy-chain.
*   **[SPI Hardware](./hardware/spi-hardware.md)** — Internal state machines and 74xx feasibility.
*   **[WiFi NIC Hardware](./hardware/esp32-nic-hw.md)** — Physical pinouts and UART bridge design.

---

## 🔬 [Penumbra Internals](./internals)
**Target:** RTL/FPGA engineers working on the Penumbra core.

Detailed design of the CPU core and internal FPGA logic.

*   **[Datapath Design](./internals/datapath.md)** — The three-bus architecture and signal flow.
*   **[Microcode Reference](./internals/microcode.md)** — Bit-level micro-word format and routine catalog.
*   **[MMU Internals](./internals/mmu-internals.md)** — TLB hardware structure and discrete logic mapping.
*   **[SDRAM Optimization](./internals/sdram-optimization.md)** — Controller strategies for bandwidth improvement.
*   **[Development Setup](./internals/setup.md)** — How to build and simulate the RTL.

---

## 🚀 [Roadmap](./TODO.md)
*   **[TODO & Roadmap](./TODO.md)** — Current tasks and long-term project goals.
