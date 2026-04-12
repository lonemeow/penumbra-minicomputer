# NetBSD Port -- Status

Penumbra port of NetBSD 10.1. Source tree is a squashed subtree
from `netbsd-10` in `netbsd/`. MD code in `sys/arch/penumbra/`.

## Current State

**Boots to single-user shell on the ISS.**  ROM autoconfig -->
bootloader --> kernel --> device drivers --> FFS root mount -->
exec `/rescue/init` --> interactive shell with console I/O.

`build.sh distribution` completes -- full userland cross-builds.
Statically-linked rescue binaries work.  Dynamic linker not yet
ported, so dynamically-linked binaries don't run.

## Boot Chain

| Stage | Location | Status |
|-------|----------|--------|
| ROM | `hw/rom/` | Done |
| Bootloader (PENBOOT.ELF) | `sys/arch/penumbra/stand/boot/` | Done |
| Kernel | `sys/arch/penumbra/penumbra/` | Boots to single-user shell |
| Userland | `build.sh distribution` | Builds; static binaries work |

ROM loads `PENBOOT.ELF` from FAT32 on SD card.  Bootloader
translates ROM boot data to bootinfo, loads kernel ELF via
libsa `loadfile()`, jumps with MMU off.

## Kernel Subsystems

### Working

- **Early boot (locore.S):** PIC bias, BSS zero, bootinfo copy
  in physical mode.  Kernel page table (L1+L2 in BSS), real
  TLB miss handler pinned before MMU enable.  Two-phase startup:
  `penumbra_init()` on 4 KB boot stack returns new SP, locore
  switches to lwp0's 12 KB USPACE before `main()`.

- **pmap / TLB:** Software-managed 2-level page table (L1-->L2).
  Full pmap interface: `pmap_enter` (demand paging), `pmap_create`/
  `pmap_destroy` (user address spaces), `pmap_activate` (L1
  re-pin + ASID on context switch), `pmap_protect`/`pmap_remove`/
  `pmap_remove_all`, `pmap_kenter_pa`/`pmap_kremove` (wired
  mappings), `pmap_extract`, `pmap_map_device`.

- **ASID management:** Generational allocator (1--255, 0=kernel).
  Stale ASIDs get fresh allocation on `pmap_activate`.  When 255
  exhausted, generation bumps, TLB flushed, allocation restarts.

- **PV lists:** Per-physical-page SLIST tracking all managed
  mappings.  `pmap_page_protect` walks PV entries.
  `pmap_clear_modify` write-protects via PV walk.

- **I-cache coherency:** `icache_invalidate()` flushes I-cache in
  `pmap_enter()` for executable page mappings.  `pmap_procwr()`
  for MI ptrace/exec code-write synchronization.  Full-flush
  only (hardware has no per-address invalidation yet).

- **Demand paging:** TLB miss/prot --> `uvm_fault()` for demand
  paging and COW.  `pcb_onfault` recovery for copyin/copyout.

- **UVM:** `uvm_md_init()`, bootinfo-driven `uvm_page_physload()`,
  `pmap_steal_memory()`.  Full UVM init: pool allocator, vmem,
  kmem, radix trees.

- **Context switching:** `cpu_switchto` (locore.S) saves/restores
  callee-saved registers.  `cpu_lwp_fork` sets up new LWP stacks.
  `lwp_trampoline` --> `lwp_startup` --> `func(arg)`.  Softint
  threads run.

- **Trap handling:** Per-vector entry stubs on the vector page,
  common trapframe save/restore, C dispatch in `trap()`.
  All 9 exception vectors.  Volatile hardware state (ESR, EPC,
  FAULT_ADDR, FAULT_STATUS) stashed into pinned scratch before
  any faultable access.  Double-fault detection.

- **Console UART:** MI `com(4)` driver via `com_pbbus.c`.
  Word-strided 32-bit registers (shift=2, width=4).  Polled I/O
  via `sc_poll_ticks=1` callout.  Userland RX+TX works.

- **Device autoconfig:** `pbbus` bridge walks `BTINFO_DEVICE`
  entries from bootinfo.  `bus_space` (map/unmap/read/write)
  via UVM + `pmap_kenter_pa`.

- **SD card block device (psd):** Polled SPI/SD driver.  MBR
  partition parsing.  bdevsw/cdevsw at major 8.  Kernel mounts
  FFS root from `psd0f`.

- **Exec / return-to-user:** `setregs()` initializes user
  trapframe.  `trap_return` handles SP banking (USP save/restore)
  and pinned-scratch ESR/EPC stash before eret.

- **Syscall dispatch:** R11=syscall number (scratch register,
  set by SYSTRAP), R1--R4=args, overflow from user stack via
  `copyin()`.  Carry-flag error convention (C=0 success, C=1
  error).  Two-value return (R1+R2) for fork/pipe.

- **Signal delivery:** `sendsig_siginfo` builds signal frame on
  user stack.  `cpu_getmcontext`/`cpu_setmcontext` for
  trapframe <--> mcontext.  `trap.c` delivers SIGSEGV/SIGBUS/
  SIGILL/SIGTRAP to user-mode processes.

- **copyin/copyout (copy.S):** Assembly with `pcb_onfault` fault
  recovery.  copyinstr/copyoutstr, ufetch/ustore (8/16/32).

- **Atomics:** RAS-based CAS for userland (`__HAVE_RAS`),
  interrupt-disable CAS for kernel.  Generic CAS-based
  inc/dec/add/and/or, no-op membars (uniprocessor).

- **SPL:** Hardware-based, reads SR.I directly (no global
  variable that desyncs on exception entry).

### Remaining Stubs

Kernel functions that will panic if reached (grep `TODO(stub)`):

- `process_read_regs`, `process_write_regs`, `process_set_pc`
- `cpu_coredump`
- `vmapbuf` / `vunmapbuf`

DDB (kernel debugger) disabled -- needs extensive MD hooks.

## Userland

`build.sh distribution` completes.  Key components:

- **CSU:** `crt0.S`, `crti.S`, `crtn.S`, `crtbegin.h`, `crtend.S`.
  `.init_array`/`.fini_array` (HAVE_INITFINI_ARRAY).
- **libc MD:** `SYS.h` (SYSTRAP/PSEUDO/RSYSCALL), `cerror.S`,
  12 custom syscall wrappers, softfloat, `makecontext`/
  `resumecontext`, `__mulsi3`, atomics (RAS + generic).
- **Libraries:** All static and shared libraries build and link.
  `MKPIC=yes` with GOT-based PIC (full 32-bit reach).
- **Dynamic linker (`ld.elf_so`):** Functional.
  `rtld_start.S`, `mdreloc.c`, RELA relocations, eager PLT
  binding.  Dynamically-linked binaries load and run.
  Library search path requires `ldconfig /lib /usr/lib`.
- **Limitations:** libpthread is minimal stubs.
  `MKCXX=no` (no C++ support).

## Kernel Config (MINIMAL)

Built at `-O0` with DIAGNOSTIC.  FFS + MSDOSFS file systems,
minimal INET networking, com(4) UART, psd(4) SD card, loop/pty/
ksyms pseudo-devices.

## SD Image + Boot

```sh
make sdimage                # boot partition only (FAT32)
make sdimage-rootfs         # boot + FFS root (minimal rescue)
make sdimage-rootfs ROOTFS_FULL=1  # boot + full distribution

make simulate SDCARD=build/boot.img
# At ROM prompt: boot sd:0,0
# At root device prompt: psd0f
```

## What's Next

1. **Root filesystem** -- `build.sh sets`, boot with full
   userland on the ISS.
2. **Remaining MD stubs** -- as the kernel reaches them.
3. **Interrupt controller** -- multiple devices with priority.
4. **SDRAM controller** -- memory subsystem for real hardware.
