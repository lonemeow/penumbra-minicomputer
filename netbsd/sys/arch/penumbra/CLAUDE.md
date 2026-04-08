# Penumbra NetBSD Port — Claude Code Context

This file provides context for the NetBSD machine-dependent port under `netbsd/sys/arch/penumbra/`. The root `CLAUDE.md` has project-wide conventions; `doc/boot/boot-process.md` has the full boot chain design.

## Overview

This is the machine-dependent ("MD") layer for porting NetBSD to Penumbra. It lives inside the NetBSD source subtree (`netbsd/`) which was added as a squashed git subtree from the `netbsd-10` branch of `https://github.com/NetBSD/src.git`.

Penumbra is ILP32, little-endian, 32-bit physical and virtual addresses, 4 KB pages. These properties are the same as MIPS o32 in little-endian mode, so the mips/evbmips port is the primary reference for structure (but all code is Penumbra-specific — no `<mips/*.h>` includes).

## Directory Layout

```
sys/arch/penumbra/
├── include/           # <machine/*.h> headers (39 files)
├── conf/              # Kernel config: std, MINIMAL, Makefile, files, majors, ldscript
├── penumbra/          # MD kernel code: locore, machdep, pmap, trap, autoconf, etc.
├── stand/
│   ├── boot/          # Bootloader (PENBOOT.ELF) — PIE, CRT self-relocator
│   └── libsa/         # Machine-dependent libsa glue (retained for reference)
```

## Build

### Prerequisites (one-time)

```sh
sh netbsd/sys/arch/penumbra/toolchain-setup.sh   # creates penumbra-unknown-none-* symlinks
cd netbsd
./build.sh -U -j4 -m penumbra tools \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest
```

### Kernel Build

Build output goes to `build/netbsd-kernel/MINIMAL/` (out of source tree).
All commands run from the project root.

```sh
# 1. Generate kernel Makefile from config (re-run after changing conf/ files)
build/netbsd-tools/bin/nbconfig \
  -b $PWD/build/netbsd-kernel/MINIMAL \
  -s $PWD/netbsd/sys \
  $PWD/netbsd/sys/arch/penumbra/conf/MINIMAL

# 2. Generate dependencies
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend

# 3. Build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL -j10
```

**Important:** Re-run step 1 after changing any `conf/` files.
The `build/netbsd-kernel/` directory is gitignored.

### Bootloader Build

```sh
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot
```

Output: `build/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF`

## Virtual Memory Layout

2G/2G user/kernel split with compact user-space layout for TLB efficiency:

```
0x0000_0000              unmapped null guard page
0x0000_1000              user text / data / bss
                         heap (grows up)
0x0400_0000  USRSTACK    stack top at 64 MB (grows down)
                         mmap region (for programs > 64 MB)
0x8000_0000              kernel VA start
0x8001_0000              kernel text (KERNEL_TEXT_BASE)
                         kernel data / bss / page tables
                         MMIO devices (mapped via pmap_map_device)
                         virtual_avail → kernel VM pool (UVM)
0xFFFF_B000  VECTOR_VA    pinned slot 0: vector page (handler + scratch)
0xFFFF_C000  SCRATCH_VA   pinned slot 3: scratch window
0xFFFF_D000  PT_L2WIN_VA  pinned slot 2: L2 window (handler)
0xFFFF_E000  PT_L1_VA     pinned slot 1: current L1 table
0xFFFF_F000              unmapped guard (catches (void*)-1 derefs)
```

**Page tables are always 2-level:** L1 (1024 entries, 4 KB) →
L2 (1024 entries each, 4 KB, 4 MB coverage).
No direct-map — all mappings are explicit PTEs.  Physical pages
without kernel VAs are accessed via the scratch window (pinned
TLB slot 3).  Naming: `PT_L1_*` for first level, `PT_L2_*` for
second level (avoids confusion with L1/L2 caches).

## Machine Headers (`include/`)

Headers fall into three categories:

**Type/ABI headers** (delegate to `sys/common_*` via compiler builtins):
`int_types.h`, `int_mwgwtypes.h`, `int_const.h`, `int_limits.h`,
`int_fmtio.h`, `types.h`, `ansi.h`, `limits.h`, `wchar_limits.h`,
`cdefs.h`, `endian.h`, `endian_machdep.h`, `bswap.h`

**Kernel interface headers** (define MD structures/macros for MI kernel):
`cpu.h` (cpu_info, curcpu), `intr.h` (IPL levels),
`frame.h` (trapframe), `pcb.h` (process control block),
`pmap.h` (page table, PTE format), `proc.h` (mdlwp/mdproc),
`vmparam.h` (VA layout, page size), `psl.h` (SR bit definitions),
`reg.h` (register sets for ptrace), `ptrace.h` (PT_GETREGS),
`lock.h` (simple locks), `rwlock.h`, `mutex.h`,
`db_machdep.h` (DDB debugger), `cpu_counter.h`,
`bus_defs.h`/`bus_funcs.h` (bus_space types),
`bootinfo.h` (bootinfo tags, ACFG_CLASS_* device classes),
`pbbus.h` (Penumbra Bus attach args),
`setjmp.h`, `profile.h`

**Boot/ELF headers:**
`elf_machdep.h` (EM_PENUMBRA, relocations),
`loadfile_machdep.h` (libsa ELF loader macros),
`aout_machdep.h`, `signal.h`, `mcontext.h`, `param.h`, `disklabel.h`

## Key Design Decisions

- **Compiler builtins:** Clang provides `__UINTPTR_TYPE__`, etc.
  Integer-type headers delegate to `sys/common_*` one-liners.
- **Reference port:** evbmips/mips for structure; all code is
  Penumbra-specific (no `<mips/*.h>` includes).
- **`-D__NetBSD__`:** Added in Makefile.penumbra because our
  target triple (`penumbra-unknown-none`) doesn't predefine it.
- **`-isystem` resource dir:** Compiler's freestanding headers
  (limits.h, stdint.h) re-added after `-nostdinc` strips them.
- **No `-msoft-float`:** Penumbra has no FPU concept at all,
  so the flag is unsupported by our LLVM backend.

## Kernel Config Files (`conf/`)

| File | Purpose |
|------|---------|
| `std.penumbra` | Machine identity, standard options (EXEC_ELF32, DEFTEXTADDR) |
| `MINIMAL` | Bare-minimum kernel config for build testing |
| `Makefile.penumbra` | MD build rules (compiler flags, link settings, genassym) |
| `files.penumbra` | MD source files and device declarations |
| `majors.penumbra` | Device major numbers |
| `kern.ldscript` | Kernel linker script |

## MD Kernel Code (`penumbra/`)

| File | Purpose |
|------|---------|
| `locore.S` | Entry point, BSS zero (phys mode), bootinfo copy, kernel page table build (L1+L2 in BSS), real TLB miss handler, per-vector trap entry stubs + common trapframe save/restore (with double-fault detection), MMU enable, TLB invalidation, scratch window, cpu_switchto, lwp_trampoline, setjmp/longjmp |
| `startup.c` | Early boot: `penumbra_init()` (phase 1 on boot stack — returns new SP), `penumbra_main()` (phase 2 on lwp0 stack — calls main()), bootinfo parsing, early UART console via scratch window, `consinit()`, `penumbra_physmem_init()`, UART remap via `pmap_map_device()` |
| `machdep.c` | Kernel runtime: `cpu_startup()`, `cpu_reboot()`, `cpu_lwp_fork()` (LWP context setup), `setregs()`, `lwp_trampoline` (extern), remaining LWP/process/signal stubs, `kcopy` |
| `mulsi3.c` | Compiler runtime: `__mulsi3` (software 32-bit multiply for LLVM libcalls) |
| `autoconf.c` | `cpu_configure()`, `cpu_rootconf()` |
| `mainbus.c` | Root bus device driver (attaches cpu + pbbus) |
| `cpu.c` | CPU device driver |
| `pbbus.c` | Penumbra Bus bridge — walks BTINFO_DEVICE entries from bootinfo, attaches child devices by class |
| `pcom.c` | Console UART driver — attaches at pbbus (ACFG_CLASS_UART), takes over cn_tab from early console |
| `psd.c` | SD card block device — SPI/SD protocol via bus_space, MBR partition parsing, bdevsw/cdevsw at major 8. Polled sector-at-a-time I/O. |
| `bus_space.c` | bus_space implementation — map/unmap via UVM + pmap_kenter_pa, read/write via volatile pointers |
| `trap.c` | Exception dispatch (all 9 vectors), TLB fault → uvm_fault() demand paging, pcb_onfault recovery for copyin/copyout, SPL stubs |
| `syscall.c` | Syscall dispatch: `syscall_intern()` + `syscall()`. R1=number, R2–R4=args, stack overflow via copyin. Carry-flag error convention (C=0 success, C=1 error). Indirect syscalls rejected with ENOSYS. |
| `pmap.c` | Software TLB management: `pmap_bootstrap()`, `pmap_steal_memory()`/`pmap_steal_page()`, `pmap_kenter_pa()`/`pmap_kremove()`, `pmap_enter()` (demand paging), `pmap_create()`/`pmap_destroy()` (user address spaces), `pmap_activate()` (L1 re-pin), `pmap_extract()`, `pmap_map_device()`, scratch window helpers. |
| `copy.S` | Assembly copyin/copyout/copyinstr/copyoutstr with pcb_onfault fault recovery, ufetch/ustore (8/16/32), user address validation |
| `genassym.cf` | Struct offset definitions for assembly code |

## Current Status

- [x] Machine headers — 39 files, sufficient for kernel compilation
- [x] Kernel config — `config MINIMAL` generates Makefile successfully
- [x] `make depend` — passes cleanly
- [x] `make` — **all .o files compile at `-O0`**
- [x] **Kernel links** — ~5 MB ELF binary at `build/netbsd-kernel/MINIMAL/netbsd`
  (DIAGNOSTIC enabled for development)
- [x] Atomics — interrupt-disable CAS (`RDSPR SR`/`DI`/op/`WRSPR SR`),
  generic CAS-based ops, no-op membars (uniprocessor)
- [x] libsa glue — `sdblk.c` (SD block device), `cons.c` (UART)
- [x] Build system — build.sh integration, out-of-tree kernel build
- [x] Assembly string functions — memcpy, memset, memcmp, strlen,
  strcmp, strcpy in `common/lib/libc/arch/penumbra/string/`
- [x] Boot loader (`PENBOOT.ELF`) — CRT self-relocator, boot data
  → bootinfo translation, kernel ELF loading via libsa `loadfile()`.
  Builds via nbmake (libsa + libkern linked as `.a` archives).
  Loads kernel at dynamic physical address, jumps with MMU off.
- [x] **locore.S early boot** — PIC bias computation, BSS zero
  and bootinfo copy in physical mode, kernel page table build
  (L1+L2 pre-allocated in BSS), real page-table-walking TLB miss
  handler installed and pinned before MMU enable, virtual jump.
  No bootstrap handler — real handler active from first instruction.
- [x] **Real pmap / TLB handler** — 2-level page table (L1→L2)
  walked by TLB miss handler via pinned slots (L1 in slot 1,
  L2 window in slot 2).  No direct-map — all mappings explicit.
  Scratch window (pinned slot 3) for C code physical page access.
  Vector page pinned at `VECTOR_VA` (0xFFFFB000), not VA 0 —
  handler uses PC-relative addressing for scratch data.
  VA 0 is unmapped (null guard page).
  Pinned slots named: `PTLB_VECTOR`, `PTLB_L1`, `PTLB_L2WIN`,
  `PTLB_SCRATCH` (sysreg.h).
- [x] **pmap_kenter_pa / pmap_kremove** — wired kernel page
  mapping via L1→L2 walk + scratch window for L2 access.
  Dynamic L2 allocation (two-phase: steal before pmap_init,
  uvm_pagealloc after).  `pmap_extract()` implemented.
  `PTE_MAKE()` macro builds PTEs from prot/flags/extra bits.
  `pmap_map_device()` for early MMIO mapping.
  Unimplemented pmap stubs panic (not silent no-ops).
- [x] **pmap_steal_memory** — steals physical pages from UVM
  physseg, maps at `virtual_avail` via scratch window + page
  table insertion.  Panics if L2 table missing (covered by
  BSS pre-allocation for early boot).
- [x] **Early console** — 16450 UART, initially pinned via
  scratch window (slot 3), permanently remapped via
  `pmap_map_device()` after `pmap_bootstrap()` but before
  `uvm_pageboot_alloc()` — `pmap_map_device` allocates from
  pmap's local `virtual_avail`, which becomes stale once UVM
  snapshots it via `pmap_virtual_space()`.
- [x] **UVM init** — `uvm_md_init()`, bootinfo parsing,
  `uvm_page_physload()` for RAM regions (excluding kernel image),
  `pmap_steal_memory()` for early page allocation.
  Full UVM init completes; boots past `main()` into
  `cpu_startup()`, autoconf, and softint thread creation.
- [x] `pmap.h` — `_LOCORE` guards, `PMAP_STEAL_MEMORY`,
  `PT_L1_*`/`PT_L2_*` naming, `PTE_MAKE()` macro
- [x] **Context switching** — `cpu_switchto` (locore.S) saves/restores
  callee-saved registers via `pcb_context` (label_t).  `cpu_lwp_fork`
  sets up new LWP kernel stacks: copies parent trapframe, wires
  `pcb_context` to resume in `lwp_trampoline`.  `lwp_trampoline`
  calls `lwp_startup(prev, newlwp)` before `func(arg)` (unlocks
  prev LWP, clears LP_RUNNING, resets SPL — required by MI).
  `_JB_*` symbolic indices for label_t slots defined in `types.h`.
  `cpu_switchto` includes SP sanity check (BREAK on corrupt
  pcb_context).  Softint threads run; boots to root device prompt.
- [x] **curlwp** — `#define curlwp (curcpu()->ci_curlwp)` in cpu.h.
  Ensures MI code and `cpu_switchto` share the same variable.
  `cpu_info_store` statically initializes `ci_curlwp = &lwp0`.
- [x] **Boot stack switch** — `penumbra_init()` returns new SP
  (lwp0 kernel stack top, 12 KB USPACE).  locore.S does
  `mov sp, r1; bl penumbra_main`.  ARM-style pattern.
  The 4 KB boot stack overflows at `-O0` + DIAGNOSTIC.
- [x] **pmap_protect / pmap_remove / pmap_unwire** —
  `pmap_protect` downgrades PTE permissions via `PTE_PROT_BITS()`
  macro (shared with `PTE_MAKE`).  `VM_PROT_NONE` delegates to
  `pmap_remove`.  `pmap_unwire` is a no-op (no SW wired bit).
- [x] **Trap handler (Stage 1 + 2)** — per-vector entry stubs on
  the vector page, common trapframe save/restore in `_trap_common`,
  C dispatch in `trap()`.  All 9 exception vectors wired.
  **TLB miss/prot dispatched to `uvm_fault()` for demand paging.**
  On fault failure: `pcb_onfault` recovery (copyin/copyout) or
  panic.  User-mode access to kernel VA rejected early.
  Double-fault detection: if ESR.S set and EPC in pinned page
  region (0xFFFFxxxx), BREAK to halt instead of infinite-looping.
- [x] **Device autoconfig (pbbus)** — bus bridge walks
  `BTINFO_DEVICE` entries from bootinfo, attaches child devices
  by ACFG_CLASS_*.  Device classes defined once in `bootinfo.h`,
  shared across ROM, bootloader, and kernel.
- [x] **bus_space** — `bus_space_map` allocates kernel VA via
  `uvm_km_alloc` + `pmap_kenter_pa` (uncached).  Read/write ops
  are volatile pointer dereferences.
- [x] **Console UART (pcom)** — attaches at pbbus
  (ACFG_CLASS_UART), maps registers via bus_space, takes over
  `cn_tab` from early boot console.  Seamless handoff — no
  output lost during transition.
- [x] **SD card block device (psd)** — polled SPI/SD driver
  attaches at pbbus for ACFG_CLASS_SD.  Full SD-SPI protocol
  (CMD0/CMD8/ACMD41/CMD58 init, CMD17 sector read) via
  bus_space.  MBR partition table parsed at attach, offsets
  applied in strategy.  bdevsw/cdevsw at major 8.
  Kernel mounts msdosfs root from psd0e (MBR partition 1)
  and reaches `init: trying /sbin/init`.
- [x] **copyin/copyout (copy.S)** — assembly implementations with
  standard NetBSD `pcb_onfault` fault recovery pattern.
  copyin/copyout use memcpy + onfault, copyinstr/copyoutstr do
  byte-loop with ENAMETOOLONG.  ufetch/ustore (8/16/32-bit)
  are leaf functions.  User address validation against
  `VM_MAXUSER_ADDRESS`.  Fault stubs clean up stack frame and
  return EFAULT.
- [x] **pmap_enter / pmap_create** — `pmap_enter()` creates
  mappings for both kernel and user pmaps.  Sets `PTE_U` for
  user, `PTE_G` for kernel, `PTE_SW_MANAGED` for UVM pages.
  `pmap_create()` allocates L1 page table, copies kernel half,
  maps L1 via `uvm_km_alloc` + `pmap_kenter_pa`.
  `pmap_destroy()` frees L1 page and pmap struct.
  `pmap_activate()` re-pins L1 in TLB slot 1 via inline WRSYS.
  `pmap_alloc_l2()` returns bool (ENOMEM-safe for `pmap_enter`,
  panic for `pmap_kenter_pa`).
- [x] **setregs / exec / return-to-user** — `setregs()` initializes
  user trapframe (entry point, SP, user-mode SR).
  `cpu_spawn_return()` is a no-op (trap return handles it).
  `lwp_trampoline` loads `md_utf` and jumps to `trap_return`
  after func(arg) returns.
  `trap_return` handles SP banking (save/restore USP via SPR),
  stashes EPC/ESR/R1/R2 in pinned vector page scratch to avoid
  TLB-miss clobbering of ESR/EPC before eret.
  Kernel successfully execs `/sbin/init` and reaches userland.
- [x] **Syscall dispatch (syscall.c)** — `SYSCALL` (vector 5)
  dispatched via `md_syscall` function pointer set by
  `syscall_intern()`.  R1=syscall number, R2–R4=register args,
  overflow from user stack via `copyin()`.  Return convention:
  R1=retval + C flag clear on success, R1=errno + C flag set on
  error.  ERESTART backs up EPC.  Indirect syscalls
  (`SYS_syscall`/`SYS___syscall`) rejected with ENOSYS.
  `userret()` called on every syscall return path.
  Init calls SYS_write + SYS_exit successfully.
- [ ] Kernel port — remaining MD stubs need real implementations
  (grep for `TODO(stub)` to find them)
- [ ] DDB — disabled, needs extensive MD hooks

## Next Steps

1. **Remaining MD stubs** — fill in `TODO(stub)` functions as
   the kernel reaches them (signals, mcontext, startlwp).
2. **Timer** — programmable timer for NetBSD hardclock() tick
3. **Interrupt controller** — multiple devices with priority
4. **Signal delivery** — sendsig_siginfo, signal trampoline,
   cpu_getmcontext/cpu_setmcontext

## Documentation

When significant kernel changes are made or milestones reached,
update `doc/netbsd/porting-status.md` (human-facing status document)
and the "Current Status" sections in both `CLAUDE.md` (project root)
and this file.  Keep all three in sync.
