# Penumbra -- TODO

Items needed for improved userland testing and interactive use.

## ISS Raw TTY Mode

The ISS terminal handling clears `ECHO` and `ICANON` but leaves
`ISIG` set, so host signal characters (Ctrl-C, Ctrl-Z, Ctrl-\)
are intercepted by the host kernel instead of being passed through
to the simulated UART.  This prevents job control, signal delivery
testing, and interactive shell use inside the simulated NetBSD.

**What's needed:**
- A `+raw` flag (or similar) that puts the terminal into full raw
  mode: clear `ISIG`, `IXON`, `ICRNL`, `OPOST` in addition to
  `ECHO`/`ICANON`
- An escape sequence to exit the simulator since Ctrl-C won't
  work (QEMU-style `Ctrl-A X`)
- Escape prefix also needs `Ctrl-A Ctrl-A` to send a literal
  Ctrl-A through
- Default (no `+raw`) keeps current behavior for bare-metal
  debugging

## Boot Arguments (ROM -> Bootloader -> Kernel)

Currently, booting to single-user mode requires manual interaction
at two prompts: the ROM monitor (`boot sd:0,0`) and the kernel
root device prompt (`psd0f`).  The full boot chain should support
passing arguments so that a single ROM command (or even default
behavior) brings the system all the way to single-user with no
further interaction.

**What's needed:**
- Extend `BTAG_BOOTARGS` (or add a new boot data tag) so the ROM
  `boot` command can pass a string (e.g., root device, boot flags)
  through to the bootloader and kernel
- Bootloader reads a config file from the FAT32 partition
  (e.g., `BOOT.CFG`) as defaults when no arguments are passed
  from the ROM
- Bootloader passes root device and boot flags to the kernel via
  bootinfo (`BTINFO_ROOTDEVICE`, `BTINFO_BOOTHOWTO`)
- Kernel's `cpu_rootconf()` uses bootinfo root device instead of
  prompting when available
- ROM `boot sd:0,0` with a properly configured `BOOT.CFG` should
  reach single-user shell with zero interaction

## SD Card Write Support / MI sdmmc Integration

The current `psd` kernel driver is a minimal custom SPI/SD
implementation that only supports reads (CMD17).  The ISS SPI
emulation already handles CMD24 writes, but the kernel driver
never issues them.

**Two possible approaches:**

1. **Minimal: add CMD24 writes to psd** -- smallest change, but
   keeps the custom single-block-at-a-time driver

2. **Proper: implement sdmmc chip functions and use MI stack** --
   implement `sdmmc_chip_functions` (primarily `exec_command()`)
   for the Penumbra SPI controller, then use NetBSD's MI
   `sdmmc`/`ld_sdmmc` drivers.  This gives multi-block I/O
   (CMD18/CMD25), proper write support, and upstream-maintained
   SD protocol handling.  The existing psd protocol code
   (command framing, response parsing, CRC) can be restructured
   into the `exec_command` shape.

Option 2 is preferred long-term but is a larger piece of work.
