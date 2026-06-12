# Penumbra Boot Process

## Overview

The Penumbra boot process has three stages, designed to work with
varying hardware configurations and to support NetBSD (or other
operating systems) without hardcoding OS assumptions into the
early boot firmware.

### Software Stack Map

```text
 ┌───────────────────────┐ 
 │  Stage 0: HW Reset    │ CPU starts at RESET_PC (0xFFFF_0000)
 └──────────┬────────────┘
            │
 ┌──────────▼────────────┐ 
 │  Stage 1: Boot ROM    │ Hardware init, Autoconfig, Disk Probe
 │  (Monitor)            │ Load PENBOOT.ELF from FAT32 partition
 └──────────┬────────────┘
            │ physical pointer to Boot Data (R1)
 ┌──────────▼────────────┐ 
 │  Stage 2: PENBOOT.ELF │ PIE self-relocation, NetBSD bootinfo setup
 │  (Loader)             │ Load netbsd kernel (MMU stays off)
 └──────────┬────────────┘
            │ physical bootinfo (R1), physical kernel entry (R2)
 ┌──────────▼────────────┐ 
 │  Stage 3: netbsd      │ Early locore.S, pmap/UVM setup
 │  (Kernel)             │ Device attachment, mount /dev/ld0f
 └──────────┬────────────┘
            │ execve()
 ┌──────────▼────────────┐ 
 │  Stage 4: /sbin/init  │ Start system services, getty
 │  (Userland)           │ Login shell
 └───────────────────────┘
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
- Load `PENBOOT.ELF` (PIE ELF) from the FAT32 root directory:
  parse ELF headers, allocate RAM for scratch and load
  destination (via `find_memory_region()`, which walks boot
  data MEMORY devices and avoids reserved areas), copy PT_LOAD
  segments, zero .bss
- Jump to loader entry point with R1 = boot data pointer

The ROM includes a minimal read-only FAT32 reader (`fat32.c`)
with a block-read callback abstraction, so it works with any
storage device — not just SD cards.  It also includes minimal
ELF32 parsing (`elf.h`) to load PIE binaries at dynamically
chosen addresses.  The `boot sd:<dev>,<cs>` monitor command
drives the full sequence.  For testing and development, `load`
and `go` commands provide raw sector reads and direct address
jumps.

**Environment:** Physical addressing, supervisor mode, no MMU.
Stack top at `0x2000`, growing down into page 1 (pages 0–1 are the
reserved low-RAM region; general RAM starts at page 2).  The boot
ROM is OS-agnostic — it knows nothing about NetBSD or any other OS.

**Boot data:** The ROM writes a Penumbra-specific boot data
structure (see below) to a well-known physical address before
jumping to the loader.

## Boot Loader

**Location:** Loaded into RAM by the ROM from `PENBOOT.ELF` on
the FAT32 boot partition.  This is a PIE (Position Independent
Executable) ELF binary.  The ROM parses ELF headers and loads
PT_LOAD segments into dynamically allocated RAM — no fixed load
address.  The loader self-relocates at startup using a CRT stub
that processes `R_PENUMBRA_32` relocations via PT_DYNAMIC.

**Responsibilities:**
- Load the kernel image from the FAT32 boot partition into RAM
  at whatever physical address is available
- Translate Penumbra boot data into NetBSD `bootinfo` structures
  (or equivalent for other OSes)
- Jump to the kernel's **physical** entry point with the **MMU
  off**: R1 = physical bootinfo pointer, R2 = physical entry
  address

**Why the kernel enables the MMU itself (not the loader):**
NetBSD (like most Unix kernels) is linked to run at a fixed
virtual address, but on Penumbra no specific *physical* address
range can be guaranteed — the amount of base RAM varies, and
future configurations may have different memory maps.  The kernel
therefore begins with a small position-independent stub in
`locore.S`: running from its physical load address, it builds the
initial two-level page table in physical mode (mapping the
kernel's load region to its link address), installs the pinned
TLB entries, enables the MMU, and jumps to its own virtual
address.  Unlike MIPS there is no untranslated KSEG window —
Penumbra's MMU is pure TLB — so this stub is the only code that
ever runs untranslated.  Keeping the whole MMU bring-up on the
kernel side gives translation state a single owner: the loader
never creates mappings the kernel would then have to discover and
tear down, and the loader stays OS-agnostic.  (See the Resolved
Decisions list below.)

**Environment:** Physical addressing throughout — MMU off at
entry and at the jump to the kernel.  Still supervisor mode.

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

Boot data and load information are passed through the entire chain via registers:
- **R1:** Holds a physical pointer to the boot data tagged list in RAM (set at every handoff).
- **R2:** Holds the physical entry point of the loaded binary. Set only at the loader → kernel handoff; **undefined at loader entry** (the ROM's `boot`/`go` jump passes R1 alone).

Each stage may append entries to the tagged list before passing the pointer in R1 forward.

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
│ Entry: type=BTAG_DEVICE           │
│   cls=MEMORY, base, size          │
│   (base RAM detected by ROM)      │
├──────────────────────────────────┤
│ Entry: type=BTAG_DEVICE           │
│   cls=UART, base, size            │
│   (built-in UART, injected)       │
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
| `BTAG_DEVICE` | 2 | `cls`, `base`, `size`, `id`, `name[16]` | Discovered or injected device (including RAM) |
| `BTAG_CONSOLE` | 3 | `dev_nth` | Console output device (index into BTAG_DEVICE entries) |
| `BTAG_BOOTDEV` | 4 | `dev_nth`, `cs`, `partition` | Boot device (see below) |

RAM regions use `BTAG_DEVICE` with `cls=ACFG_CLASS_MEMORY` — no
separate memory tag.  This means base RAM (detected by the ROM),
extension RAM (discovered via autoconfig), and any future memory
source all use the same entry type.  Consumers find RAM by
filtering on the MEMORY device class.

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

The loader maps Penumbra boot data onto NetBSD `bootinfo` tags
(`BTINFO_MEMORY`, `BTINFO_CONSOLE`, `BTINFO_BOOTPATH`, …) and adds
loader-generated entries with no BTAG counterpart: `BTINFO_SYMTAB`
(kernel symbol table, for DDB) and `BTINFO_KERNBASE`.  A `root=`
line in `boot.cfg` on the boot partition becomes
`BTINFO_ROOTDEVICE` — how shipped images reach single-user with no
prompts.  The authoritative tag list is the loader source,
`netbsd/sys/arch/penumbra/stand/boot/boot.c`.

## Storage Layout (SD Card)

Target layout for the ULX3S SD card:

```
┌─────────────────────────────────────┐
│ MBR (sector 0)                      │  Partition table only (no boot code)
├─────────────────────────────────────┤
│ Partition gap (sectors 1–2047)      │  Unused (available for future use)
├─────────────────────────────────────┤
│ Partition 1: FAT32                  │  Boot loader + kernel image
│   PENBOOT.ELF                       │  Boot loader (PIE ELF, loaded by ROM)
│   netbsd [.gz]                      │  Kernel (loader searches netbsd, netbsd.gz)
├─────────────────────────────────────┤
│ Partition 2: UFS/FFS                │  Root filesystem
│   NetBSD root (/, /etc, /bin, ...)  │
├─────────────────────────────────────┤
│ Partition 3: swap (optional)        │
└─────────────────────────────────────┘
```

**Why FAT32 for the boot partition?** The ULX3S has a WiFi module (ESP32) that can mount FAT32 and serve files. This allows updating the kernel remotely without needing a NetBSD-aware tool — just copy a new kernel image to the FAT32 partition.

## Resolved Decisions

1. **Boot loader location:** `PENBOOT.ELF` (PIE ELF) on the
   FAT32 boot partition (root directory).  The ROM mounts FAT32,
   parses ELF headers, loads PT_LOAD segments into dynamically
   allocated RAM, and jumps to entry.  The loader self-relocates
   at startup.  No fixed load address — works with any RAM map.

2. **Boot data passing:** R1 = physical pointer to boot data. Tagged list format for extensibility (type + size per entry, skip unknown tags).

3. **Direct-mapped region:** No hardware bypass (unlike MIPS KSEG0). The kernel handles its own MMU setup in early `locore.S` using a 2-level page table built in physical mode.

4. **Boot data entry alignment:** 4-byte aligned (ILP32 word size). Entry `size` is always a multiple of 4.

5. **Device indexing:** Autoconfig assigns sequential device indices (0, 1, 2, ...). `BTAG_DEVICE` carries the index; `BTAG_BOOTDEV` references boot device by index. Kernel maps boot device → hardware by matching indices.

6. **SD card naming:** ROM monitor uses `sd:<controller>,<cs>[:<partition>]` syntax. `controller` is the index among SD-class devices (not the global device index), `cs` is the chip-select pin (0 or 1), optional `partition` (1-based) enables partition-relative LBA addressing. The ROM translates the per-class controller index to the global device index for `BTAG_BOOTDEV`.

7. **SD card controller:** SPI master in SPI mode (`spi.sv`), autoconfigured as `CLASS_SD` — the ROM's boot-device probe matches that class, not `CLASS_SPI`. Polled transfers (single-byte and FIFO burst). Testbench SD emulator backed by disk image file (`+sdcard=`). See `doc/system/devices/spi.md`.

8. **Kernel link address:** The kernel (Stage 3) can be linked at any virtual address. To support loading at an arbitrary physical address, the kernel's early entry code must be position-independent (PIC) so it can execute in physical mode before the MMU is enabled.

9. **Initial MMU setup:** The bootloader is not required to pre-map kernel pages. The kernel's entry code is responsible for building its own page tables in physical mode, installing its exception handlers, and enabling the MMU before jumping to its virtual link address.

10. **Boot data physical location:** The ROM places the boot data at physical address `0x0000_0040` (page 0, immediately following the exception vector table).

## Open Questions

(None currently identified.)
