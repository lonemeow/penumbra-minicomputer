# Penumbra Boot Process

## Overview

The Penumbra boot process has three stages, designed to work with
varying hardware configurations and to support NetBSD (or other
operating systems) without hardcoding OS assumptions into the
early boot firmware.

```
 ROM                   physical mode, no MMU
   │                   hardware init, FAT32 file load
   ▼
 Boot loader           physical mode → enables MMU, loads kernel
   │
   ▼
 Kernel                runs with MMU on, at fixed virtual address
```

## Boot ROM

**Location:** On-chip ROM at `0xFFFF_0000` (CPU reset vector).

**Responsibilities:**
- Hardware initialization (trap vectors, UART)
- RAM detection via bus-fault probing
- Bus autoconfig enumeration (discovers SPI controller, other
  peripherals)
- Boot device detection (SPI controller → SD card)
- Populate boot data structure with hardware discovery results
  (RAM, peripherals, boot device)
- Mount the first FAT32 partition on the boot SD card
- Load the `LOADER` file from the FAT32 root directory into RAM
- Jump to loader with R1 = boot data pointer

The ROM includes a minimal read-only FAT32 reader (`fat32.c`)
with a block-read callback abstraction, so it works with any
storage device — not just SD cards.  The `boot sd:<dev>,<cs>`
monitor command drives the full sequence.  For testing and
development, `load` and `go` commands provide raw sector reads
and direct address jumps.

**Environment:** Physical addressing, supervisor mode, no MMU.
Stack in low RAM (page 2+).  The boot ROM is OS-agnostic — it
knows nothing about NetBSD or any other OS.

**Boot data:** The ROM writes a Penumbra-specific boot data
structure (see below) to a well-known physical address before
jumping to the loader.

## Boot Loader

**Location:** Loaded into RAM by the ROM from `LOADER` on the
FAT32 boot partition.  This is a full-size binary (no partition
gap size constraints), loaded at `0x00010000` by default.

**Responsibilities:**
- Load the kernel image from the FAT32 boot partition into RAM
  at whatever physical address is available
- Enable the MMU and create initial TLB mappings:
  - Map the kernel's physical load address to its link virtual
    address (at least enough pages for the kernel to reach its
    own TLB setup code)
  - Optionally pre-map additional regions the kernel needs
    during very early init
- Translate Penumbra boot data into NetBSD `bootinfo` structures
  (or equivalent for other OSes)
- Jump to the kernel entry point with MMU enabled,
  R1 = bootinfo pointer (virtual)

**Why enable MMU here (not in kernel)?**
NetBSD (like most Unix kernels) is linked to run at a fixed
virtual address to avoid runtime relocation.  On Penumbra, we
cannot guarantee that any specific *physical* address range is
available — the amount of base RAM varies, and future
configurations may have different memory maps.  By enabling the
MMU in the bootloader:
- The kernel can be loaded at whatever physical address is
  available
- The bootloader creates a virtual mapping at the kernel's link
  address
- The kernel starts executing in a known virtual address space
- No relocation support is needed in the kernel

This matches how MIPS NetBSD boots (firmware sets up initial
TLB, kernel takes over pmap).

**Initial TLB setup:** The bootloader must map at least enough
of the kernel for it to reach its own TLB initialization code
in `locore.S`.  Unlike MIPS (which has hardwired KSEG0/KSEG1
segments that bypass TLB), Penumbra's MMU is pure TLB — there
is no untranslated window.  The kernel's very first instructions
execute through TLB entries set up by the bootloader.  How many
pages need pre-mapping depends on how much code runs before the
kernel establishes its own mappings; this will be determined
during the port.

**Environment:** Starts in physical mode.  Enables MMU before
jumping to kernel.  Still supervisor mode.

## Kernel

**Location:** Mapped at fixed virtual address (e.g., `0x8000_0000`).

**Responsibilities:**
- Parse bootinfo, initialize pmap/UVM
- Set up full virtual memory (kernel pages, direct-map region)
- Initialize devices, mount root filesystem (UFS/FFS partition on SD card)
- Start init

**Environment:** Virtual addressing (MMU on), supervisor mode.

## Boot Data

### Passing Convention

Boot data is passed through the entire chain via **R1** (first argument register), which holds a physical pointer to the boot data structure in RAM. Each stage may append entries before passing the pointer forward.

### Penumbra Boot Data (firmware-level)

The ROM allocates and populates a Penumbra-specific boot data structure in physical RAM. This structure is OS-agnostic and used throughout the firmware boot chain. Each stage may append new entries (e.g., stage 1 adds boot device details).

The structure is a **tagged list** — a sequence of variable-length entries, each with a type tag and a size field. This allows forward and backward compatibility: old consumers skip unknown tags, new producers add new tags without breaking old consumers.

```
┌──────────────────────────────────┐
│ Boot Data Header                 │
│   magic: 0x50454E42 ("PENB")    │
│   version: 1                     │
│   total_size: N                  │
├──────────────────────────────────┤
│ Entry: type=BTAG_MEMORY          │
│   size: (entry size incl header) │
│   ram_base, ram_size             │
├──────────────────────────────────┤
│ Entry: type=BTAG_CONSOLE         │
│   size: ...                      │
│   uart_base, uart_type           │
├──────────────────────────────────┤
│ Entry: type=BTAG_BOOTDEV         │
│   size: ...                      │
│   device_type, ...               │
├──────────────────────────────────┤
│ ... more entries ...             │
├──────────────────────────────────┤
│ Entry: type=BTAG_END             │
│   size: 8                        │
└──────────────────────────────────┘
```

Each entry has a common 8-byte header:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `type` | Tag identifying the entry type (BTAG_xxx) |
| 4 | 4 | `size` | Total size of this entry in bytes (including this header) |

Consumers walk the list by advancing by `size` bytes per entry. Unknown tags are skipped. The list is terminated by `BTAG_END`.

**Defined tags:**

| Tag | Value | Payload | Description |
|-----|-------|---------|-------------|
| `BTAG_END` | 0 | (none) | Terminates the list |
| `BTAG_MEMORY` | 1 | `base`, `size` | Physical RAM region (multiple allowed) |
| `BTAG_DEVICE` | 2 | `cls`, `base`, `size`, `id`, `name[16]` | Discovered or injected device |
| `BTAG_CONSOLE` | 3 | `dev_nth` | Console output device (index into BTAG_DEVICE entries) |
| `BTAG_BOOTDEV` | 4 | `dev_nth`, `cs`, `partition` | Boot device (see below) |

Additional tags as hardware grows (cache geometry, etc.).

**Device indexing and boot device identification:**

Each device discovered by autoconfig receives a sequential **device index** (0, 1, 2, ...) in discovery order. `BTAG_DEVICE` entries carry this index along with the device class and MMIO base address. `BTAG_BOOTDEV` references the boot device by `dev_index` plus the CS pin and partition number used. This allows the kernel to map the boot device back to its hardware:

```
BTAG_DEVICE  dev_index=0  cls=SPI  base=0xFF001000   (onboard)
BTAG_DEVICE  dev_index=1  cls=SPI  base=0xFF002000   (external bus)
BTAG_BOOTDEV dev_index=0  cs=0  partition=1
```

The kernel looks up `dev_index=0` → SPI controller at `0xFF001000`, CS pin 0, partition 1 → that's where root lives.

**SD card naming convention (ROM monitor):**

The ROM monitor uses `sd:<controller>,<cs>` syntax where `<controller>` is the index among SD-class devices (0 = first SD controller, 1 = second, etc.) and `<cs>` is the chip-select pin (0 or 1). For example, `sd:0,0` means "first SD controller, CS0". This is unambiguous even when multiple SPI controllers exist (e.g., FPGA onboard + external bus). Note that this is a per-class index, not the global device index used in `BTAG_BOOTDEV` — the ROM translates between the two.

**Entry alignment:** All entries are 4-byte aligned (matching Penumbra's ILP32 word size). The `size` field is always a multiple of 4. Consumers advance by `size` bytes and can assume word-aligned access.

**Location in RAM:** The ROM places the boot data at the top of the first detected RAM page that isn't reserved for the vector table or trap stack. The exact address depends on RAM layout, but R1 always points to it, so consumers don't need to know a fixed address.

### NetBSD Bootinfo Translation

The boot loader translates Penumbra boot data into NetBSD's
`bootinfo` structure (`sys/arch/*/include/bootinfo.h`).  This
keeps the firmware chain OS-independent — a different OS or
bare-metal program can ignore the translation step.

TODO: Map Penumbra boot data fields to NetBSD `bootinfo` tags (`BTINFO_MEMORY`, `BTINFO_CONSOLE`, `BTINFO_BOOTDEV`, etc.).

## Storage Layout (SD Card)

Target layout for the ULX3S SD card:

```
┌─────────────────────────────────────┐
│ MBR (sector 0)                      │  Partition table only (no boot code)
├─────────────────────────────────────┤
│ Partition gap (sectors 1–2047)      │  Unused (available for future use)
├─────────────────────────────────────┤
│ Partition 1: FAT32                  │  Boot loader + kernel image
│   LOADER                            │  Boot loader (loaded by ROM)
│   PENUMBRA                          │  Kernel image (loaded by LOADER)
├─────────────────────────────────────┤
│ Partition 2: UFS/FFS                │  Root filesystem
│   NetBSD root (/, /etc, /bin, ...)  │
├─────────────────────────────────────┤
│ Partition 3: swap (optional)        │
└─────────────────────────────────────┘
```

**Why FAT32 for the boot partition?** The ULX3S has a WiFi module (ESP32) that can mount FAT32 and serve files. This allows updating the kernel remotely without needing a NetBSD-aware tool — just copy a new kernel image to the FAT32 partition.

## Resolved Decisions

1. **Boot loader location:** `LOADER` file on the FAT32 boot
   partition (root directory, 8.3 name).  The ROM mounts FAT32
   directly — no intermediate stage in the partition gap.
   This avoids PIC/relocation issues and removes size constraints.

2. **Boot data passing:** R1 = physical pointer to boot data. Tagged list format for extensibility (type + size per entry, skip unknown tags).

3. **Direct-mapped region:** No hardware bypass (unlike MIPS KSEG0). Bootloader pre-maps kernel pages in TLB. Kernel takes over TLB management early in `locore.S`.

4. **Boot data entry alignment:** 4-byte aligned (ILP32 word size). Entry `size` is always a multiple of 4.

5. **Device indexing:** Autoconfig assigns sequential device indices (0, 1, 2, ...). `BTAG_DEVICE` carries the index; `BTAG_BOOTDEV` references boot device by index. Kernel maps boot device → hardware by matching indices.

6. **SD card naming:** ROM monitor uses `sd:<controller>,<cs>[:<partition>]` syntax. `controller` is the index among SD-class devices (not the global device index), `cs` is the chip-select pin (0 or 1), optional `partition` (1-based) enables partition-relative LBA addressing. The ROM translates the per-class controller index to the global device index for `BTAG_BOOTDEV`.

7. **SD card controller:** SPI master in SPI mode (sim_spi.sv, CLASS_SPI). Byte-at-a-time polled transfers. Autoconfigured. Testbench SD emulator backed by disk image file (`+sdcard=`). See `doc/boot/spi-controller.md`.

## Open Questions

1. **Kernel link address:** What virtual address should the kernel be linked at? `0x8000_0000` is conventional for MIPS (KSEG0). We need to pick a Penumbra convention.

2. **Minimum pre-mapped pages:** How many kernel pages must the bootloader map before jumping? Depends on how much code runs before `locore.S` establishes its own TLB entries. To be determined during the port.

3. **Kernel image format:** Raw binary? ELF? a.out? NetBSD traditionally uses ELF with a boot header. The stage 2 loader needs to parse it.

4. **Boot data physical location:** ROM places boot data in a known RAM area (not a fixed address — depends on detected RAM). R1 points to it. Should the boot data be at the bottom of usable RAM (simple, but kernel must know to avoid it) or at the top (out of the way, but requires knowing RAM size to find it)?
