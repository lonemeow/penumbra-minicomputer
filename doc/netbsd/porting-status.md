# NetBSD Port — Status and Progress

## Goal

Port NetBSD 10.1 to the Penumbra minicomputer. The NetBSD source tree is included as a squashed git subtree from the `netbsd-10` branch (`netbsd/`). Machine-dependent code lives in `netbsd/sys/arch/penumbra/`.

Reference port: evbmips (ILP32 little-endian MIPS with software-managed TLB). All code is Penumbra-specific — no `<mips/*.h>` includes.

## Boot Chain

The full boot chain is documented in `doc/boot/boot-process.md`. Summary:

| Stage | Location | Status | Description |
|-------|----------|--------|-------------|
| ROM | `hw/rom/` | **Done** | Hardware init, autoconfig, SD card, FAT32, ELF loader |
| Bootloader | `netbsd/sys/arch/penumbra/stand/boot/` | **Done** | PIE ELF (PENBOOT.ELF), loaded from FAT32 root by ROM. Translates boot data → bootinfo, loads kernel via libsa `loadfile()`, jumps with MMU off |
| Kernel | `netbsd/sys/arch/penumbra/penumbra/` | **Execs /sbin/init, syscalls work** | locore.S, pmap, traps, context switching, console, exec, syscall dispatch — all working |

The ROM loads `PENBOOT.ELF` from the FAT32 partition, which in turn loads the kernel ELF at a dynamic physical address and jumps to it. No intermediate stage — the bootloader handles ELF parsing, bootinfo setup, and kernel handoff directly.

## Machine Headers

**Status: Done** — 39 headers in `netbsd/sys/arch/penumbra/include/`, sufficient for full kernel compilation.

The headers define Penumbra as:
- **ILP32** — `int`, `long`, and pointers are all 32-bit
- **Little-endian** — `_BYTE_ORDER = _LITTLE_ENDIAN`
- **4 KB pages** — matches the Penumbra MMU (`PGSHIFT=12`)
- **4-byte alignment** — `__ALIGNBYTES=3`
- **16 general registers** — R0=zero, R14=SP, R15=PC
- **2G/2G user/kernel split** — kernel at `0x8001_0000`, user from `0x0000_1000`

Most integer-type headers delegate to NetBSD's `sys/common_*` headers, which use compiler builtins provided by clang.

## Kernel Status

**The kernel execs `/sbin/init` and dispatches syscalls.** Full boot chain: ROM autoconfig → bootloader → kernel → device drivers → msdosfs root mount → exec init → userland syscalls. Init calls `SYS_write` and `SYS_exit` successfully.

### What Works

- **Early boot (locore.S):** PIC bias computation, BSS zero, bootinfo copy in physical mode. Builds kernel page table (L1 + L2 in BSS), installs real page-table-walking TLB miss handler, enables MMU. Two-phase startup: `penumbra_init()` runs on 4 KB boot stack and returns new SP; locore switches to lwp0's 12 KB kernel stack before calling `main()`.

- **Virtual memory (pmap.c):** Software-managed 2-level page table (L1→L2). TLB miss handler walks page tables via pinned slots. `pmap_kenter_pa`/`pmap_kremove` for wired kernel mappings. Dynamic L2 allocation (steal before pmap_init, uvm_pagealloc after). `pmap_protect`/`pmap_remove`/`pmap_unwire`. Scratch window for physical page access.

- **Context switching:** `cpu_switchto` saves/restores callee-saved registers via `pcb_context`. `lwp_trampoline` calls `lwp_startup(prev, newlwp)` then `func(arg)`. SP sanity check catches corrupt pcb_context early.

- **Trap handling (Stage 1):** Per-vector entry stubs, common trapframe save/restore, C dispatch in `trap()`. All 9 exception vectors wired. Double-fault detection (ESR.S + EPC in pinned region) prevents infinite fault loops.

- **Console:** Early boot uses 16450 UART via TLB scratch window, then `pmap_map_device()`. The `pcom` driver takes over `cn_tab` during autoconf using a proper `bus_space` mapping.

- **Device autoconfiguration:** `pbbus` bridge walks `BTINFO_DEVICE` entries from bootinfo and attaches child devices. ROM autoconfig results (device class, MMIO base, size) are passed through the bootloader to the kernel. `bus_space` (map/unmap/read/write) implemented for memory-mapped I/O.

- **SD card block device (psd):** Polled SPI/SD driver attaches at pbbus for `ACFG_CLASS_SD`. Implements full SD-SPI protocol (init, sector read) via `bus_space`. MBR partition table parsed at attach; partition offsets applied in strategy. Provides `bdevsw`/`cdevsw` at major 8. Kernel successfully mounts msdosfs root from `psd0e` (MBR partition 1).

- **curlwp:** Defined as `curcpu()->ci_curlwp` macro so MI code and `cpu_switchto` share the same variable. `cpu_info_store` statically initializes it to `&lwp0`.

- **Demand paging:** TLB miss/prot faults dispatch to `uvm_fault()` for demand paging and COW. `pcb_onfault` recovery for copyin/copyout kernel faults. User-mode access to kernel VA rejected early.

- **copyin/copyout (copy.S):** Assembly implementations with `pcb_onfault` fault recovery. copyinstr/copyoutstr byte-loop. ufetch/ustore (8/16/32). User address validation against `VM_MAXUSER_ADDRESS`.

- **Exec and return-to-user:** `setregs()` initializes user trapframe. `lwp_trampoline` → `trap_return` handles SP banking (USP save/restore) and pinned-scratch ESR/EPC stash to prevent TLB-miss clobbering before eret. Kernel execs `/sbin/init` and reaches userland.

- **Syscall dispatch:** `SYSCALL` instruction (vector 5) dispatches through `md_syscall` function pointer. R1=syscall number, R2–R4=args, overflow from user stack via `copyin()`. Return convention: R1=retval on success (C flag clear), R1=errno on error (C flag set). ERESTART backs up EPC to re-execute SYSCALL. Indirect syscalls (`SYS_syscall`/`SYS___syscall`) explicitly rejected with ENOSYS until implemented.

- **DIAGNOSTIC:** Enabled for development — all KASSERT checks active.

- **Signal delivery working.** `sendsig_siginfo` builds signal frame on user stack, redirects to handler with LR = libc sigtramp. `cpu_getmcontext`/`cpu_setmcontext`/`cpu_mcontext_validate` implemented. `trap.c` delivers SIGSEGV/SIGBUS/SIGILL/SIGTRAP to user-mode processes via `trapsignal()`. `cpu_lwp_setprivate` writes TP (R12) to trapframe (`__HAVE_CPU_LWP_SETPRIVATE`). `/rescue/sh` starts and exits cleanly on the ISS.

### What's Next

1. **Remaining MD stubs** — fill in `TODO(stub)` functions as the kernel reaches them (grep `TODO(stub)`).
2. **Interrupt controller** — multiple devices with priority encoding.
3. **Memory subsystem** — SDRAM controller, bus interface.

### Key Design Decisions

- **No direct-map:** All kernel memory is mapped via explicit PTEs. Physical pages without kernel VAs are accessed via the scratch window (pinned TLB slot 3). This differs from MIPS (KSEG0/KSEG1) but matches the 74xx-feasible TLB design.

- **UART mapping timing:** `pmap_map_device()` must run before any `uvm_pageboot_alloc()` call, because the latter snapshots `virtual_avail` into UVM's own variable via `pmap_virtual_space()`. After that point, pmap's local `virtual_avail` is stale and must not be used for allocation.

- **Boot stack switch:** ARM-style pattern — C init function returns new SP to assembly, which switches and calls the continuation. The 4 KB boot stack cannot survive `main()` at `-O0` with DIAGNOSTIC.

## Build

### Prerequisites (one-time)

```sh
sh netbsd/sys/arch/penumbra/toolchain-setup.sh
cd netbsd
./build.sh -U -j4 -m penumbra tools \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest
```

### Kernel Build

```sh
# Generate Makefile (re-run after conf/ changes)
build/netbsd-tools/bin/nbconfig \
  -b $PWD/build/netbsd-kernel/MINIMAL \
  -s $PWD/netbsd/sys \
  $PWD/netbsd/sys/arch/penumbra/conf/MINIMAL

# Build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL -j10
```

### Bootloader Build

```sh
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot
```

### SD Image + Boot

```sh
sw/tools/mksdimage.sh -o build/boot.img \
  -2 build/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF \
  -k build/netbsd-kernel/MINIMAL/netbsd

make simulate SDCARD=build/boot.img
# At ROM prompt: boot sd:0,0
```

### NetBSD Tree Modifications

Key files modified outside `sys/arch/penumbra/`:
- `build.sh` — penumbra in `valid_MACHINE_ARCH` table
- `share/mk/bsd.own.mk` — TOOLCHAIN_MISSING, HAVE_LLVM, MACHINE_GNU_PLATFORM
- `share/mk/bsd.endian.mk` — penumbra in little-endian list
