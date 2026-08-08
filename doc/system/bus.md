# Penumbra Bus — System Programmer's Reference

The Penumbra Bus connects the CPU complex to memory and I/O peripherals.
This document describes the programmer-visible behavior: the physical
address map, how bus faults are delivered, how autoconfig enumerates
devices, and how interrupts are reported.

Signal-level protocol (4-phase handshake, timing) lives in
[hardware/bus-protocol.md](../hardware/bus-protocol.md).
Electrical details of the config chain live in
[hardware/autoconfig-hardware.md](../hardware/autoconfig-hardware.md).
The `BUSCTL` register bits are defined in
[sysregs.md](./sysregs.md#device-4-bus-bus-controller).

---

## Address Map

The physical address space uses a fixed layout decoded from the top
address bits. RAM occupies the low addresses; I/O and boot ROM
occupy the top.

```
0x0000_0000 ┌─────────────────────┐
            │ System RAM          │  Cached, hardwired at base 0
            │  (size probed)      │  Probed upward until bus fault
            ├─────────────────────┤
            │ Expansion RAM       │  Autoconfigured, assigned by ROM
            ├─────────────────────┤
            │ (unmapped)          │  Bus fault if accessed
0xFDFF_FFFF └─────────────────────┘
0xFE00_0000 ┌─────────────────────┐
            │ Config Space (32 B) │  Autoconfig registers
            │                     │   — only when CFG_EN set
0xFE00_001F └─────────────────────┘
            │ (unmapped)          │
0xFEFF_FFFF └─────────────────────┘
0xFF00_0000 ┌─────────────────────┐
            │ I/O Region (16 MB)  │  Always uncached (C=0)
            │   0xFF00_0000 UART  │  Hardwired console
            │   (elsewhere in     │  Autoconfigured — any
            │    region)          │  4 KB-aligned free range
0xFFFE_FFFF └─────────────────────┘
0xFFFF_0000 ┌─────────────────────┐
            │ Boot ROM (64 KB)    │  Always uncached (C=0), hardwired
0xFFFF_FFFF └─────────────────────┘
```

- **Reset vector:** `0xFFFF_0000` (base of boot ROM). CPU starts
  here with MMU in flat mode (`MMUCR.M=0`). This is a hardwired PC
  reset value, not part of the vector table.
- **Exception vector table:** 16 words at physical `0x0000_0000`.
  Each entry is a 32-bit handler address (not an instruction).
  The CPU fetches the handler via MMU bypass, then jumps.
- **Unmapped regions:** accessing an unmapped address raises
  `EXC_BUS_FAULT` (vector 0) — see [Bus Faults](#bus-faults).

### Minimum RAM Requirement

The boot ROM requires **at least two pages (8 KB) of RAM starting at
physical address 0** to function:

- Page 0 (`0x0000_0000`–`0x0000_0FFF`) holds the exception vector table
  (the ROM installs a bus-fault-ignore handler here before probing),
  the scratch save area used by the bus-fault probe routine, and the
  boot data tagged list (see [boot-protocol.md](./boot-protocol.md)).
- Page 1 (`0x0000_1000`–`0x0000_1FFF`) holds the ROM's working stack
  while it runs autoconfig.

A platform with less than 8 KB of base RAM cannot boot, because the
probe that discovers additional RAM itself needs these two pages
working. The RAM probe (see below) extends the usable region upward
from the 2-page floor.

### I/O Peripheral Map

Only the console UART has a fixed I/O address. Every other peripheral
receives its base address at boot from the autoconfig protocol, which
allocates from any free range in the 16 MB I/O region (or from free
RAM-region space for memory-class devices).

| Base         | Size | Device   | Hardwired / AC | Notes                        |
|--------------|------|----------|:--------------:|------------------------------|
| `0xFF00_0000`| 4 KB | UART     | HW             | Console — see [devices/uart.md](./devices/uart.md) |
| *(assigned)* | 4 KB | SPI      | AC             | SD card, flash               |
| *(assigned)* |64 KB | Ethernet | AC             | Wiznet / ESP32 NIC           |
| *(assigned)* | 4 KB | GPIO     | AC             | General-purpose I/O          |

HW = hardwired (fixed address). AC = autoconfigured at boot.

Each simple peripheral receives a **4 KB page-aligned region**
regardless of how many registers it uses. This guarantees each device
occupies exactly one MMU page, so cacheability and protection can be
set per device without aliasing.

---

## Bus Faults

When a transfer addresses unmapped space, no device responds and the
master's timeout counter raises `EXC_BUS_FAULT` (vector 0). The MMU
latches:

- `FAULT_ADDR` — virtual address of the faulting access
- `FAULT_STATUS` — `type = FAULT_BUS`, plus access info (R/W/X, size)

The kernel reads these via [sysregs](./sysregs.md#device-0-mmu).
`bus_space_peek`/`bus_space_poke` use this mechanism to probe for
optional devices: install a fault-returning trap handler, perform the
probe, and catch the fault if nothing responded.

**RAM sizing.** Base RAM (at physical 0) has an unknown installed
size, but is guaranteed to be at least 8 KB (see above). The boot ROM
probes upward in page-sized steps from the 2-page floor until a bus
fault occurs; the last successful read is the top of system RAM.
Expansion RAM is assigned elsewhere in free physical space via
autoconfig.

---

## Device Discovery (Autoconfig)

Autoconfigurable devices power up in an unconfigured state. The boot
ROM enumerates them via a daisy-chained config protocol inspired by
Amiga Zorro II/III, then records each device (base address, class, ID,
name) into the boot data structure for the loader and kernel to
consume. See [boot-protocol.md](./boot-protocol.md#boot-data) for the
`BTAG_DEVICE` format.

### Hardwired Devices

A small set of devices have fixed base addresses set at design time
and are always present; the ROM does not discover them.

| Device   | Base          | Size     | Why hardwired                                 |
|----------|---------------|----------|-----------------------------------------------|
| System RAM | `0x0000_0000` | probed | Needs to be usable before autoconfig runs     |
| UART     | `0xFF00_0000` | 4 KB     | Console for autoconfig diagnostics            |
| Boot ROM | `0xFFFF_0000` | 64 KB    | CPU reset vector                              |

The boot ROM injects synthetic `BTAG_DEVICE` entries for these so the
kernel sees a uniform device list.

### Config Space

When `BUSCTL.CFG_EN` is set, a 32-byte window at `0xFE00_0000` becomes
active. Reads/writes go to the **first unconfigured device** in the
daisy chain; the device's eventual base address is not yet involved.

| Address       | R/W | Name        | Description                                      |
|---------------|:---:|-------------|--------------------------------------------------|
| `0xFE00_0000` | R   | `CFG_CLASS` | Device class code (see table below)              |
| `0xFE00_0004` | R   | `CFG_SIZE`  | Required address space in bytes (power of 2)     |
| `0xFE00_0008` | R   | `CFG_ID`    | Manufacturer + product ID (0 = generic)          |
| `0xFE00_000C` | R   | `CFG_NAME0` | Name bytes 0–3 (packed LE, null-padded)          |
| `0xFE00_0010` | R   | `CFG_NAME1` | Name bytes 4–7                                   |
| `0xFE00_0014` | R   | `CFG_NAME2` | Name bytes 8–11                                  |
| `0xFE00_0018` | R   | `CFG_NAME3` | Name bytes 12–15                                 |
| `0xFE00_001C` | W   | `CFG_BASE`  | Assigned base address; writing enables the device|

When no unconfigured device remains, a read of `CFG_CLASS` produces a
bus fault (timeout). Software uses this to detect the end of the chain.

### Device Class Codes

The class identifies a **family** of similar devices, and — for some
classes — the base register protocol that family shares. Generic
firmware (ROM, loader) can use any device whose class defines a base
protocol it understands, with no device-specific driver needed.

| Value | Name             | Base protocol                  | Notes                             |
|-------|------------------|--------------------------------|-----------------------------------|
| 0     | `CLASS_UNKNOWN`  | (none)                         | Needs OS driver matched by `CFG_ID` |
| 1     | `CLASS_MEMORY`   | (none — plain address space)   | RAM / ROM. Used for expansion RAM |
| 2     | `CLASS_UART`     | NS16450 register layout        | Boot ROM can use as console       |
| 3     | `CLASS_SPI`      | Penumbra SPI master            | Generic SPI controller            |
| 4     | `CLASS_SD`       | Penumbra SPI master, SD wired  | SD card attached via SPI          |
| 5     | `CLASS_NIC`      | Penumbra NIC protocol          | ESP32 / Wiznet network adapter    |
| 6     | `CLASS_DISPLAY`     | Penumbra display registers     | Character-cell console + optional framebuffer |
| 7     | —                   | —                              | Unassigned                        |
| 8     | `CLASS_USBHC`       | Penumbra USB host-controller   | Local USB host (low / full speed) |
| 9–255 | —                   | —                              | Reserved for future protocols     |

**Extended devices.** A device with extra capabilities (e.g., an SPI
controller with a DMA engine) still reports the base class and
implements the base registers. The boot ROM uses only the base
registers. Later, the OS driver reads `CFG_ID` to detect the specific
variant and enables extended features. Any `CLASS_SPI`/`CLASS_SD`
device is bootable even if the OS hasn't loaded a variant-specific
driver yet.

**Minimum protocols are per-class.** A class *may* define a minimum
register protocol — a base subset a generic consumer can drive without a
device-specific driver. When a class defines one, every device claiming
that class **must** implement it after reset, so firmware always finds
the minimum even if software had switched the device into a richer mode;
extra capabilities are opt-in behind `CFG_ID` (as with `CLASS_SPI` /
`CLASS_SD`). Not every class defines a minimum protocol — some families
(e.g., network adapters) share no subset worth standardizing, and the OS
matches a specific driver through `CFG_ID`; the class still serves as the
family identifier. `CLASS_UNKNOWN` is for a device that fits no family at
all; a genuinely new family gets a fresh class number — its minimum
protocol, if any, fixed by its first device — rather than stretching an
existing class's contract.

### CFG_EN Toggle Protocol

**Software must deassert and reassert `CFG_EN` after configuring each
device.** This is a hard requirement of the protocol.

Writing `CFG_BASE` latches the device's address and moves it to the
enabled state, but the `cfg` chain does **not** propagate to the next
device until `CFG_EN` has been toggled. Without the toggle, the write
that configured device N could still be on the bus when device N+1
starts listening — a race that would misconfigure the next device.

**Sequence per device:**

1. Read `CFG_CLASS`, `CFG_SIZE`, `CFG_ID`, `CFG_NAME0..3`.
2. Write `CFG_BASE` with the chosen base address.
3. `WRSYS SYSDEV_BUS, BUSCTL, 0` — clear `CFG_EN`.
4. `WRSYS SYSDEV_BUS, BUSCTL, CFG_EN` — set `CFG_EN` again.
5. Repeat from step 1 until `CFG_CLASS` read bus-faults.

See [autoconfig-hardware.md](../hardware/autoconfig-hardware.md) for
the electrical implementation.

### Expansion RAM

Expansion RAM cards participate in the same autoconfig protocol. They
report `CLASS_MEMORY` and their installed size in `CFG_SIZE`. The ROM
assigns them a contiguous range above system RAM (or in any other
available window) and records each region as a `BTAG_DEVICE` entry
with `cls=CLASS_MEMORY`. The kernel finds RAM by filtering boot-data
devices on this class — base RAM and expansion RAM use the same entry
type.

This mirrors the Amiga model: Chip RAM (fixed at 0) + Zorro Fast RAM
(autoconfigured above).

### Autoconfig Boot Sequence

Reference flow followed by the boot ROM:

```c
#define BUSCTL_RST    1
#define BUSCTL_CFG_EN 2

/* Assert bus reset, delay >= 100 µs, release, enable config mode. */
wrsys(SYSDEV_BUS, BUSCTL, BUSCTL_RST);
delay_us(100);
wrsys(SYSDEV_BUS, BUSCTL, BUSCTL_CFG_EN);

install_trap_handler(VEC_BUS_FAULT, _trap_bus_ignore);

while (ndevs < MAX_DEVS) {
    uint32_t cls = bus_probe_read(0xFE00_0000);   /* CFG_CLASS */
    if (bus_probe_faulted()) break;               /* chain empty */

    uint32_t size = read32(0xFE00_0004);
    uint32_t id   = read32(0xFE00_0008);
    char name[16]; read_name(name, 0xFE00_000C);

    uint32_t base = allocate_base(cls, size);
    write32(0xFE00_001C, base);                   /* CFG_BASE */

    /* Toggle CFG_EN to advance the chain. */
    wrsys(SYSDEV_BUS, BUSCTL, 0);
    wrsys(SYSDEV_BUS, BUSCTL, BUSCTL_CFG_EN);

    append_btag_device(cls, id, base, size, name);
    ndevs++;
}

restore_trap_handler(VEC_BUS_FAULT);
wrsys(SYSDEV_BUS, BUSCTL, 0);                     /* exit config mode */
```

The 100 µs reset pulse is defined in
[hardware/bus-protocol.md](../hardware/bus-protocol.md); the `cfg`
chain semantics are in
[hardware/autoconfig-hardware.md](../hardware/autoconfig-hardware.md).

### Software-Triggered Bus Reset

`BUSCTL.RST` is a sticky R/W bit. Writing 1 asserts the bus reset
signal — all autoconfigurable devices return to their unconfigured
state. Writing 0 deasserts it. Software controls the pulse width,
since the external bus has no shared clock. Use cases:

- Re-running autoconfig after hot-plug
- OS reboot without a full power cycle

---

## Interrupt Model

Penumbra has **no interrupt controller**. Interrupts reach the CPU
over two separate inputs with distinct vectors:

| CPU input    | Source             | Vector           | Notes                        |
|--------------|--------------------|------------------|------------------------------|
| `timer_irq`  | Internal timer     | `VEC_TIMER` (1)  | Sysreg device 7; own vector  |
| `irq`        | Shared bus wire-OR | `VEC_EXT_IRQ` (9)| All bus devices share        |

Every bus device (onboard or external) drives its `/IRQ` output onto a
single wired-OR line. When the line asserts, the CPU takes
`VEC_EXT_IRQ`, and the kernel handler **polls each registered device's
status register** to identify the source (PCI-style shared interrupt).
Each device exposes its own IRQ status bits — e.g., UART has the IIR
register (per 16450 spec), SPI has `IRQ_STATUS`.

**Why no controller.** One signal vs. N lines minimizes bus wiring,
avoids autoconfig complexity for IRQ assignment, and is trivially
feasible in discrete 74xx logic (one pull-up resistor, one
open-collector gate per device). It also means onboard (FPGA) and
external (discrete bus) devices are architecturally identical —
critical because today's FPGA-internal devices become external boards
in the future discrete build.

The timer has its own dedicated vector so kernel preemption tick
handling does not pay the polling cost of shared-IRQ dispatch.

### Shared-IRQ Dispatch Pattern

A correct shared-IRQ handler for `VEC_EXT_IRQ` **must** walk every
registered device handler on each interrupt — not short-circuit on
first claim — because the wire-OR line means any non-claimant could
still be asserting. Short-circuiting would strand the asserting device
and produce an IRQ storm.

Recommended structure (any OS):

1. Register a per-device handler under `VEC_EXT_IRQ`.
2. On each IRQ, iterate all registered handlers; each handler inspects
   its device's status register and returns whether it saw pending work.
3. Unclaimed interrupts should increment a spurious-interrupt counter
   rather than being ignored silently — they indicate either a bug or
   unknown hardware on the shared line.

(NetBSD reference implementation: `intr_establish_xname()` and
`intr_dispatch()` in `netbsd/sys/arch/penumbra/penumbra/intr.c`.)
