# NetBSD Port -- Status

Penumbra port of NetBSD 10.1. Source tree is a squashed subtree
from `netbsd-10` in `netbsd/`. MD code in `sys/arch/penumbra/`.

## Current State

**Boots to single-user shell with full dynamically-linked userland
on real ULX3S hardware --- on either the Penumbra/1 or Penumbra/2 CPU
core --- and on the ISS.**  ROM autoconfig --> bootloader --> kernel
--> device drivers --> FFS root mount --> exec `/sbin/init` -->
interactive shell with console I/O.

`build.sh distribution` completes -- full userland cross-builds.
Both statically-linked rescue binaries and dynamically-linked
system binaries (`/bin/sh`, `/bin/ls`, `ldd`, etc.) work.

## Boot Chain

| Stage | Location | Status |
|-------|----------|--------|
| ROM | `hw/rom/` | Done |
| Bootloader (PENBOOT.ELF) | `sys/arch/penumbra/stand/boot/` | Done |
| Kernel | `sys/arch/penumbra/penumbra/` | Boots to single-user shell |
| Userland | `build.sh distribution` | Full userland; dynamic binaries work |

ROM loads `PENBOOT.ELF` from FAT32 on SD card.  Bootloader
translates ROM boot data to bootinfo, reads `boot.cfg` from
FAT32 via libsa `perform_bootcfg()` (root device, etc.),
loads kernel ELF via libsa `loadfile()`, jumps with MMU off.
Kernel auto-selects root device from `BTINFO_ROOTDEVICE` in
bootinfo — `boot sd:0,0` reaches single-user with no prompts.

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
  All exception vectors handled, including `VEC_ARITH` (10,
  divide-by-zero) → `SIGFPE`/`FPE_INTDIV`.  Volatile hardware
  state (ESR, EPC, FAULT_ADDR, FAULT_STATUS) stashed into pinned
  scratch before any faultable access.  Double-fault detection.

- **Console UART:** MI `com(4)` driver via `com_pbbus.c`.
  Word-strided 32-bit registers (shift=2, width=4).  IRQ-driven
  via `intr_establish_xname()` on the shared /IRQ line.
  Userland RX+TX works; kernel `cnputc` path remains polled by
  MI convention.

- **Shared-IRQ dispatch (`intr.c`):** Penumbra has no interrupt
  controller.  Devices wire-OR their `/IRQ` outputs onto the
  single CPU external interrupt input.  `intr_establish_xname()`
  links a handler into a registry; `intr_dispatch()` (called
  from `trap.c` EXC_EXT_IRQ with `ci_idepth` bracketing) walks
  every handler and calls it.  Walk is unconditional --- any
  non-claimant could still be asserting, so short-circuiting
  would strand it.  Per-handler `struct evcnt` under group
  `"shared irq"` exposes counts to `vmstat -i`; separate
  spurious counter for unclaimed IRQs.  IPL is binary (SR.I is
  one bit) so handlers effectively run at IPL_HIGH regardless
  of the requested level.

- **Device autoconfig:** `pbbus` bridge walks `BTINFO_DEVICE`
  entries from bootinfo.  `bus_space` (map/unmap/read/write)
  via UVM + `pmap_kenter_pa`.

- **SD card block device (ld0):** pmci driver + MI `sdmmc(4)` stack.
  MBR partition parsing.  Kernel mounts FFS root from `ld0f`.

- **USB host controller (pusbhc):** full `CLASS_USBHC` HCD under
  the MI USB stack, `dev/ic/sl811hs.c`-style.  Bus methods,
  software root hub (`usbroothub`), and the transfer engine:
  control, interrupt, and bulk pipes, one transaction at a time
  through a ready queue; interrupt endpoints NAK-pace at
  `bInterval` via gated SOF interrupts, control/bulk retry
  round-robin.  `uhub0` explores the port and enumerates an
  attached device end to end (verified against the ISS device
  model).  `umass` + `scsibus` + `sd` attach the simulated USB
  mass-storage disk; an FFS filesystem mounts through it and
  round-trips file data (ISS-verified).  `cdce` networking is
  next.

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

- **DDB (minimal-useful subset):** On-demand entry via serial
  BREAK (the MI `cn_check_magic` default magic — Ctrl-A B in
  ISS raw mode delivers the BREAK condition) or automatic
  entry on `panic`.  Working commands: `ps`, `show lwp`,
  `show registers`, `bt`, `bt /t <lwp_addr>`, `c`.  The
  per-LWP backtrace seeds from the LWP's `pcb_context`
  (saved R13/R14 = LR/SP after `cpu_switchto`); a
  prologue-scanning unwinder recognizes `SUB r14, #imm` and
  `STW rN, [r14, #off]` for callee-saved registers and walks
  frames until it hits an asm boundary
  (`cpu_switchto`, `lwp_trampoline`, `_trap_common`, the
  pinned vector page).  Symbol table is shipped by the
  bootloader via `BTINFO_SYMTAB` (libsa `LOAD_SYM`) and
  registered with `ksyms_addsyms_elf` in `cpu_startup()`
  after `pmap_map_kernel_tail` installs PTEs for the
  post-`_end` symtab region.  Kernel-mode TLB and bus faults
  during `db_read_bytes` longjmp through `db_recover`, so
  the debugger can safely probe unmapped VAs without
  panicking.  **Out of scope** (deliberately, for now):
  single-step (`s`/`si`/`so`), breakpoint planting (`b`),
  real disassembler — `x/i` prints raw words.

### Remaining Stubs

Kernel functions that will panic if reached (grep `TODO(stub)`):

- `process_read_regs`, `process_write_regs`, `process_set_pc`
- `cpu_coredump`
- `vmapbuf` / `vunmapbuf`

## Userland

`build.sh distribution` completes.  Key components:

- **CSU:** `crt0.S`, `crti.S`, `crtn.S`, `crtbegin.h`, `crtend.S`.
  `.init_array`/`.fini_array` (HAVE_INITFINI_ARRAY).
- **libc MD:** `SYS.h` (SYSTRAP/PSEUDO/RSYSCALL), `cerror.S`,
  12 custom syscall wrappers, softfloat, `makecontext`/
  `resumecontext`, `__mulsi3`, atomics (RAS + generic).
- **Libraries:** All static and shared libraries build and link.
  `MKPIC=yes` with GOT-based PIC (full 32-bit reach).
- **Dynamic linker (`ld.elf_so`):** Fully functional.
  `rtld_start.S`, `mdreloc.c`, RELA relocations, eager PLT
  binding, JUMP_SLOT, TLS Variant I (`__HAVE___LWP_GETTCB_FAST`).
  Dynamically-linked binaries (including `/bin/sh`, `/bin/ls`,
  `ldd`) load and run end-to-end with full userland, on real
  hardware and on the ISS.
- **C++ / ATF:** `MKCXX=yes`, `MKLIBCXX=yes`.  libunwind ported
  (in-tree, built into libc).  libc++ and libcxxrt link as shared
  libraries.  libatf-c available for the ATF test suite.
- **Limitations:** libpthread is minimal stubs.

## Kernel Config (MINIMAL)

Built at `-O2` with DIAGNOSTIC (NetBSD's stock kernel default).  FFS + MSDOSFS file systems,
minimal INET networking, com(4) UART, pmci + MI sdmmc (ld0),
pusbhc + MI USB stack (usb/uhub/umass/scsibus/sd),
loop/pty/ksyms pseudo-devices.

## SD Image + Boot

```sh
make sdimage                # boot partition only (FAT32)
make sdimage-rootfs         # boot + full FFS root + benchmark ELFs on FAT32

make simulate SDCARD=build/boot.img
# At ROM prompt: boot sd:0,0
# Boots to single-user shell (root=ld0f via boot.cfg)
```

Rootfs images include `boot.cfg` on the FAT32 partition with
`root=ld0f`.  The bootloader reads this via libsa
`perform_bootcfg()` and passes `BTINFO_ROOTDEVICE` to the
kernel, which auto-selects the root device without prompting.

## What's Next

See `doc/TODO.md` for detailed descriptions.

1. **ATF regression tests** -- build rootfs with test suite, run
   `t_swapcontext` and other ATF tests on the ISS.
2. **Root filesystem** -- `build.sh sets` to create installable sets,
   boot with full userland on the ISS.
3. **SPI FIFO + IRQ-driven pmci** -- extend `pmci.c` with
   FIFO-burst data transfers and `intr_establish_xname()`
   wakeups.
4. **Remaining MD stubs** -- fill in `TODO(stub)` functions as
   the kernel reaches them.
