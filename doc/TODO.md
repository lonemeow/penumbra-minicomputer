# Penumbra -- TODO

Items needed for improved userland testing and interactive use.

## ISS Raw TTY Mode — DONE

Implemented: `+raw` flag, Ctrl-A escape prefix (X=exit, C=CPU
state, H=help), `make simulate RAW=1`.

## Boot Arguments — DONE

Implemented: bootloader reads `boot.cfg` from FAT32 via libsa
`perform_bootcfg()`, `root=ld0f` emits `BTINFO_ROOTDEVICE`,
kernel `cpu_rootconf()` auto-selects root device.
`make sdimage-rootfs` includes `boot.cfg` automatically.

## SPI v2 Hardware — DONE

Implemented: `spi.sv` (real) and `sim_spi.sv` (sim) with hardware
TX/RX FIFO and transfer engine. 7-register interface: CAP, STATUS,
CONTROL, DATA, XFER_COUNT, IRQ_STATUS, IRQ_ENABLE.

## Kernel IRQ Dispatch — DONE

Implemented: `netbsd/sys/arch/penumbra/penumbra/intr.c` shared-IRQ
dispatcher. `com(4)` UART is IRQ-driven.

## MI sdmmc Kernel Driver — DONE (polled)

Implemented: `netbsd/sys/arch/penumbra/penumbra/pmci.c` host
controller driver. Kernel mounts FFS root from `ld0f`.

---

## Roadmap

### Phase 3.5: SPI FIFO + IRQ-driven pmci

Extend `pmci_exec_command` in `netbsd/sys/arch/penumbra/penumbra/pmci.c`
to use the SPI v2 FIFO-burst engine for the 512-byte data phase, with
`intr_establish_xname()` wakeups on XFER_DONE (large-FIFO) or
TX_THRESH/RX_THRESH (small-FIFO, discrete build). Polled baseline
remains the reference implementation.

### Phase 4: Hardware MUL/DIV

Implement hardware multiplier and divider in the ALU. Currently
trapped as illegal instructions and emulated in software
(`__mulsi3` in libc).

### Phase 5: FPU

Add a floating-point unit to the ALU. Currently using soft-float.
