# NetBSD Port — Status and Progress

## Goal

Port NetBSD 10.1 to the Penumbra minicomputer. The NetBSD source tree is included as a squashed git subtree from the `netbsd-10` branch (`netbsd/`). Machine-dependent code lives in `netbsd/sys/arch/penumbra/`.

## Boot Chain

The full boot chain is documented in `doc/boot/boot-process.md`. Summary:

| Stage | Location | Status | Description |
|-------|----------|--------|-------------|
| Stage 0 (ROM) | `hw/rom/` | **Done** | Hardware init, autoconfig, SD card, monitor |
| Stage 1 | `netbsd/sys/arch/penumbra/stand/boot/` | **In progress** | Load stage 2 from FAT32 partition |
| Stage 2 | (planned) | Not started | ELF loader, MMU enable, kernel handoff |
| Kernel | (planned) | Not started | locore.S, pmap, traps, console, root mount |

## Machine Headers

**Status: Done** — 16 headers in `netbsd/sys/arch/penumbra/include/`, sufficient for libsa standalone programs.

The headers define Penumbra as:
- **ILP32** — `int`, `long`, and pointers are all 32-bit
- **Little-endian** — `_BYTE_ORDER = _LITTLE_ENDIAN`
- **4 KB pages** — matches the Penumbra MMU (`PGSHIFT=12`)
- **4-byte alignment** — `__ALIGNBYTES=3`
- **16 general registers** — R0=zero, R14=SP, R15=PC (reflected in `mcontext.h`)

Most integer-type headers delegate to NetBSD's `sys/common_*` headers, which use compiler builtins provided by clang. This avoids duplicating MIPS-specific conditionals.

Reference port: evbmips (ILP32 little-endian MIPS with software-managed TLB).

## Stage 1 Bootloader Plan

Stage 1 is loaded by the ROM from the partition gap (sectors 1–2047, ~1 MB). It receives R1 = pointer to boot data tagged list (defined in `hw/rom/bootdata.h`).

**Responsibilities:**
1. Walk boot data to find boot SPI controller and partition info
2. Initialize SD card via SPI (or reuse ROM's init — TBD)
3. Parse MBR to find FAT32 boot partition
4. Use NetBSD libsa `dosfs.c` to read files from FAT32
5. Load stage 2 (`/boot/boot2`) into RAM
6. Jump to stage 2 with boot data pointer

**Key components needed:**
- `stand/libsa/sdblk.c` — block device strategy (SPI register-level SD reads → libsa interface)
- `stand/boot/conf.c` — wire `dosfs` filesystem + `sdblk` device into libsa
- `stand/boot/boot.c` — main program
- `stand/boot/Makefile` — clang cross-compilation (similar to `hw/rom/Makefile`)

**Open question:** Whether to duplicate the SPI/SD driver code from `hw/rom/sdcard.c` or pass function pointers from ROM via boot data. Duplication is simpler and avoids coupling; function pointers save space but create a ROM ↔ bootloader ABI contract.

## Future Milestones

### Stage 2 Bootloader
- Parse ELF kernel image
- Enable MMU with initial TLB mappings
- Translate Penumbra boot data → NetBSD `bootinfo` structures
- Jump to kernel at virtual address

### Kernel (locore.S + early init)
- Exception vectors, trap dispatch
- `pmap` — software TLB management (64-entry 2-way SA, WRSYS/RDSYS interface)
- Console driver — NS16450 UART at `0xFF000000` (com(4) compatible)
- Root mount from UFS/FFS partition on SD card

### Kernel (full port)
- Scheduler tick (needs timer peripheral — not yet in hardware)
- Device autoconf (bus_space, bus_dma)
- User-mode support (SYSCALL/ERET, USP banking)
- Networking (Wiznet Ethernet — future hardware)

## Build Notes

The NetBSD subtree is large. We don't use `build.sh` yet — the bootloader is built with a standalone Makefile using the Penumbra clang/lld toolchain, similar to the ROM build.

Compile test for headers:
```sh
ln -sfn arch/penumbra/include netbsd/sys/machine
build/llvm/bin/clang --target=penumbra-unknown-none -ffreestanding -nostdinc \
  -D_STANDALONE -I netbsd/sys -c test.c -o /dev/null
rm netbsd/sys/machine
```
