# Penumbra NetBSD MD Port — Claude Code Context

Navigation aid for work in the NetBSD machine-dependent layer at
`netbsd/sys/arch/penumbra/`. The root `CLAUDE.md` has project-wide
conventions.

**Source of truth — read these before assuming anything:**
- `doc/system/netbsd/porting-status.md` — current kernel/userland
  status, working subsystems, remaining stubs. **Always use this for
  status, not this file.**
- `doc/system/netbsd/design-notes.md` — architectural decisions for
  the port.
- `doc/system/boot-protocol.md` — full ROM → bootloader → kernel
  handoff design.
- `doc/system/{abi,mmu,sysregs,bus}.md` — ABI, MMU, sysreg, and bus
  specs that the MD layer implements.
- `doc/TODO.md` — outstanding work and roadmap.

The NetBSD source tree (`netbsd/`) is a squashed subtree from the
`netbsd-10` branch of `https://github.com/NetBSD/src.git`. Penumbra
is ILP32 little-endian, 32-bit physical and virtual addresses, 4 KB
pages — same shape as MIPS o32 in little-endian mode, so
`evbmips/mips` is the primary reference for code structure (but no
`<mips/*.h>` includes — all code is Penumbra-specific).

## Directory layout

```
sys/arch/penumbra/
├── include/           # <machine/*.h> headers (~51 files)
├── conf/              # Kernel config: std, GENERIC(.DEBUG), MINIMAL, files, majors, ldscript
├── penumbra/          # MD kernel code: locore, machdep, pmap, trap, autoconf, ...
└── stand/
    ├── boot/          # Bootloader (PENBOOT.ELF) — PIE, CRT self-relocator
    └── libsa/         # MD libsa glue (retained for reference)
```

## Build

### Prerequisites (one-time)

```sh
sh netbsd/sys/arch/penumbra/toolchain-setup.sh   # penumbra-unknown-netbsd-* symlinks
cd netbsd
./build.sh -U -j4 -m penumbra tools \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest
```

### Kernel build

Driven by stock `build.sh kernel=...`; runs `nbconfig` + `depend` +
`all` itself. `-U` sets MKUNPRIVED, which build.sh requires of a
non-root build; add `-u` (MKUPDATE) only when an incremental run is
wanted, since it skips the initial `cleandir`.

```sh
cd netbsd
MAKECONF=${PWD}/../minimal-mk.conf ./build.sh -j10 -U -m penumbra \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  kernel=GENERIC.DEBUG
cd ..
```

Kernel lands at `build/netbsd-obj/sys/arch/penumbra/compile/GENERIC.DEBUG/netbsd`
(the path is structural — `build.sh` derives it from `KERNOBJDIR` and
mirrors the `sys/arch/penumbra/compile/` source tree under `-O`).

### Bootloader build

```sh
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand obj
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot
```

Output: `build/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF`.
**Create the objdir first** (`obj` target) — bmake silently builds
in-tree without it.

## Virtual memory layout

2G/2G user/kernel split with a compact user-space layout for TLB
efficiency:

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
0xFFFF_A000  PT_USER_L1_VA pinned slot 4: current user L1 (re-pinned per pmap_activate)
0xFFFF_B000  VECTOR_VA    pinned slot 0: vector page (handler + scratch)
0xFFFF_C000  SCRATCH_VA   pinned slot 3: scratch window
0xFFFF_D000  PT_L2WIN_VA  pinned slot 2: L2 page-table window
0xFFFF_E000  PT_KERN_L1_VA pinned slot 1: kernel L1 (permanent, pinned at boot)
0xFFFF_F000              unmapped guard (catches (void*)-1 derefs)
```

Page tables are always 2-level: L1 (1024 entries × 4 B) → L2 (1024
entries × 4 B, 4 MB coverage per L2). **No direct-map** — all
mappings are explicit PTEs. Physical pages without kernel VAs are
accessed via the scratch window (pinned slot 3).

Naming: `PT_L1_*` for first level, `PT_L2_*` for second level — keeps
page-table levels visually distinct from L1/L2 caches. Pinned slot
names (in `sysreg.h`): `PTLB_VECTOR`, `PTLB_KERN_L1`, `PTLB_L2WIN`,
`PTLB_SCRATCH`, `PTLB_USER_L1`.  The fast TLB miss handler branches
on the faulting VA's MSB and walks either kernel L1 (slot 1, never
reprogrammed after boot) or current user L1 (slot 4, re-pinned per
`pmap_activate`) — SH-4-style split walker.  User pmaps therefore
hold *only* user-VA entries; no kernel-half mirroring.

## Machine headers (`include/`)

Three categories of headers:

- **Type/ABI** — delegate to `sys/common_*` via compiler builtins:
  `int_types.h`, `int_mwgwtypes.h`, `int_const.h`, `int_limits.h`,
  `int_fmtio.h`, `types.h`, `ansi.h`, `limits.h`, `wchar_limits.h`,
  `cdefs.h`, `endian.h`, `endian_machdep.h`, `bswap.h`.
- **Kernel interface** — MD structures/macros consumed by MI code:
  `cpu.h`, `intr.h`, `frame.h`, `pcb.h`, `pmap.h`, `proc.h`,
  `vmparam.h`, `psl.h`, `reg.h`, `ptrace.h`, `lock.h`, `rwlock.h`,
  `mutex.h`, `db_machdep.h`, `cpu_counter.h`, `bus_defs.h`,
  `bus_funcs.h`, `bootinfo.h`, `pbbus.h`, `setjmp.h`, `profile.h`.
- **Boot/ELF** — `elf_machdep.h` (EM_PENUMBRA, relocations),
  `loadfile_machdep.h` (libsa ELF loader macros), `aout_machdep.h`,
  `signal.h`, `mcontext.h`, `param.h`, `disklabel.h`.

## Kernel config (`conf/`)

| File | Purpose |
|------|---------|
| `std.penumbra` | Machine identity, standard options (EXEC_ELF32, DEFTEXTADDR) |
| `GENERIC` | Full device set, consistency checks off (demo/perf) |
| `GENERIC.DEBUG` | GENERIC + DIAGNOSTIC — the development default |
| `MINIMAL` | Smallest bootable config; fast smoke builds, documents the floor |
| `Makefile.penumbra` | MD build rules (compiler flags, link, genassym) |
| `files.penumbra` | MD source files and device declarations |
| `majors.penumbra` | Device major numbers |
| `kern.ldscript` | Kernel linker script |

## MD kernel code (`penumbra/`)

| File | Purpose |
|------|---------|
| `locore.S` | Entry, BSS zero, bootinfo copy, kernel page-table build, real TLB miss handler, per-vector trap entry stubs + `_trap_common` (with double-fault detection), MMU enable, I/D cache enable, TLB invalidation, scratch window, `cpu_switchto`, `lwp_trampoline`, setjmp/longjmp |
| `startup.c` | Early boot: `penumbra_init()` (phase 1 on boot stack — returns new SP), `penumbra_main()` (phase 2 on lwp0 stack — calls main()), bootinfo parsing, early UART console via scratch window, `consinit`, `penumbra_physmem_init`, UART remap via `pmap_map_device` |
| `machdep.c` | Runtime: `cpu_startup`, `cpu_reboot`, `cpu_lwp_fork`, `setregs`, remaining LWP/process/signal stubs, `kcopy`, `cpu_idle` (spl0 for timer), timer/delay |
| `mulsi3.c` | `__mulsi3` software 32-bit multiply (LLVM libcall) |
| `autoconf.c` | `cpu_configure`, `cpu_rootconf` |
| `mainbus.c` | Root bus driver (attaches cpu + pbbus) |
| `cpu.c` | CPU device driver |
| `pbbus.c` | Penumbra Bus bridge — walks `BTINFO_DEVICE` entries from bootinfo, attaches children by ACFG class |
| `com_pbbus.c` | MI `com(4)` bus attachment for pbbus — ACFG_CLASS_UART, stride=2/width=4, IRQ-driven via `intr_establish_xname`, `comcnattach1` console registration |
| `pcom.c` | Legacy console UART (unused — replaced by `com_pbbus.c`) |
| `pmci.c` | SD/MMC host controller driver — implements MI `sdmmc_chip_functions` over SPI v2. Polled byte-at-a-time. Bounded timeouts: `SD_RESP_RETRIES=8` (Ncr), `SD_DATA_TOKEN_RETRIES=100000` (~130 ms Nac at FAST), `SD_BUSY_RETRIES=500000` (~650 ms Nbr). CMD9/CMD10 wire-byte reverse + CRC-slot shift. Attaches `sdmmc → ld_sdmmc → ld` |
| `pusbhc.c` | USB host-controller driver for `CLASS_USBHC` — MI `usbd_bus_methods`/`usbd_pipe_methods` over the transaction engine, software root hub, one transaction at a time through a ready queue. Interrupt endpoints launch at `bInterval` through a gated-SOF frame wait (every launch, not only NAK retries); errored transactions retry on an exponential frame backoff. Line-capture and port-state debug views under `machdep.pusbhc.*` |
| `pdisplay.c` | `wsdisplay` driver for `CLASS_DISPLAY` — emulops write 16-bit {attr, glyph} cells through the CELLS aperture, with a RAM shadow backing the copy ops (device reads are uncached MMIO). Cursor is the device's own; `mapchar` resolves Unicode to the ROM's CP437 glyphs via `wsfont_map_unichar()` |
| `bus_space.c` | bus_space: map/unmap via UVM + `pmap_kenter_pa`. Single-value read/write/barrier ops live as macros in `include/bus_funcs.h` so each call inlines a volatile pointer deref. `bus_dma_*` panic stubs (no DMA engine) |
| `trap.c` | Exception dispatch (all 9 vectors), TLB fault → `uvm_fault()` demand paging, `pcb_onfault` recovery, hardware-based SPL (SR.I derived). EXC_EXT_IRQ → `intr_dispatch()` |
| `intr.c` | Shared-IRQ dispatch — `intr_establish_xname`/`_disestablish`/`_dispatch`, per-handler `LIST_HEAD` registry with `struct evcnt` under group `"shared irq"`, spurious counter |
| `syscall.c` | Syscall dispatch: `syscall_intern` + `syscall()`. R11=nr (scratch, set by SYSTRAP), R1–R4=args, stack overflow via copyin. Carry-flag error convention (C=0 success, C=1 error). Indirect syscalls rejected with ENOSYS |
| `pmap.c` | Software TLB: `pmap_bootstrap`, `pmap_steal_memory`/`_page`, `pmap_kenter_pa`/`_kremove`, `pmap_enter` (demand paging), `pmap_create`/`_destroy` (user pmaps), `pmap_activate` (L1 re-pin + MMUCR ASID), generational ASID allocator, `pmap_remove_all`, `pmap_extract`, `pmap_map_device`, scratch-window helpers |
| `copy.S` | Assembly copyin/copyout/copyinstr/copyoutstr with `pcb_onfault` fault recovery, ufetch/ustore (8/16/32), user-address validation |
| `db_machdep.c` | DDB MD glue — `Debugger`/`cpu_Debugger`, `db_read_bytes`/`db_write_bytes`, `db_regs[]`, `db_active` |
| `db_trace.c` | Prologue-scanning stack unwinder — recognizes `SUB r14,#imm` and `STW rN,[r14,#off]` for callee-saved. Termination markers stop the walk at asm boundaries (`cpu_switchto`, `lwp_trampoline`, `_trap_common`, pinned vector page) |
| `db_disasm.c` | Stub DDB disassembler — `x/i` prints raw 32-bit words, no instruction decode (minimal-useful DDB scope) |
| `cache_perfctrs.c` | Cache perfctr sysctl interface — L1d/L1i/L2 hit/miss counters under `machdep.cache.*`, read live via `RDSYS` |
| `cpu_perfctrs.c` | CPU perfctr sysctl interface — cycles, retired insns, and exec/fetch/load stall counters under `machdep.cpu.*` |
| `genassym.cf` | Struct offset definitions for assembly code |

## Notable implementation choices

These are non-obvious decisions worth remembering when editing the
port. Behavior details belong in source comments and
`doc/system/netbsd/{porting-status,design-notes}.md`; this section
captures the *why* for choices that would otherwise look odd.

- **Hardware-based SPL.** `splraise`/`splhigh`/etc. read `SR.I`
  directly — no global `cpl` variable. Avoids the classic
  cpl-desync-on-exception-entry bug, and IPL is binary anyway
  (SR.I is one bit), so all non-NONE levels collapse to "disable".
- **No interrupt controller.** Devices wire-OR `/IRQ` onto the
  single CPU input. `intr_dispatch()` walks every registered
  handler unconditionally — wire-OR semantics mean a simultaneous
  asserter would get stranded if we short-circuited on first claim.
- **Vector page at `VECTOR_VA` (0xFFFFB000), not VA 0.** Handler
  uses PC-relative addressing for scratch data. VA 0 is unmapped
  as a null-pointer guard.
- **`_trap_common` snapshots volatile hardware state (ESR, EPC,
  FAULT_ADDR, FAULT_STATUS) into pinned scratch before any
  faultable stack access.** TLB misses during trapframe allocation
  would otherwise clobber the original exception context.
- **Double-fault detection.** If ESR.S is set and EPC lies in the
  pinned page region (0xFFFFxxxx), `_trap_common` BREAKs instead of
  infinite-looping.
- **Two-phase startup.** `penumbra_init()` runs on a 4 KB boot
  stack, returns the new SP (top of lwp0's 12 KB USPACE) in R1, and
  `locore.S` does `mov sp, r1; bl penumbra_main`. ARM-style. The
  4 KB boot stack overflows at `-O0 + DIAGNOSTIC` if we tried to
  stay on it.
- **I-cache + master CACHE_CTRL.** Caches are enabled in `locore.S`
  *after* MMU bring-up. `PTE.C` only declares cacheability per
  page; the master `CACHE_CTRL.ENABLE` must also be set, or no
  caching happens regardless of PTE bits. `icache_invalidate()` is
  called from `pmap_enter()` for executable mappings and from
  `pmap_procwr()` for MI ptrace/exec sync (full-flush only —
  hardware has no per-address invalidation yet).
- **Syscall return.** R1=rval[0] + R2=rval[1] + carry-flag clear on
  success; R1=errno + carry-flag set on error. Two-value return
  needed by `fork` (R2 distinguishes parent/child) and `pipe` (two
  fds). `md_child_return` sets R1=0, R2=1 for the fork child.
  ERESTART backs up EPC.
- **What the display console can draw is fixed by wscons, not by the
  font.** The VT100 emulation decodes no UTF-8 and can select only
  ASCII, Latin-1 and DEC special graphics, so a program writing to
  `ttyE0` reaches ASCII, the ~50 Latin-1 characters CP437 shares, and 12
  DEC graphics (box drawing plus one shade, `ESC(0 'd'`). The block and
  shade glyphs are present in the ROM font but no escape sequence names
  them; reaching the full 256 needs the mmap'd CELLS aperture, which
  `pdisplay_mmap` does not implement.
- **`bus_dma_*` are panic stubs.** Penumbra has no DMA engine;
  `SMC_CAPS_DMA` is never set, so `pmci`/`sdmmc` always take the
  PIO path. If you see a `bus_dma_*` panic, a driver is requesting
  DMA without checking caps.
- **CMD24 polls for the data-response token.** Not a single read —
  the SD-spec 0..8-byte Ncrc gap means a fixed-offset read would
  miss the response. See `pmci.c` write path.
- **R5 holds the ucontext pointer at signal trampoline entry.**
  Callee-saved by ABI, so the libc `__sigtramp_siginfo_2`
  trampoline can find it for its `setcontext()` call.
- **Symbol table is shipped by the bootloader** via `BTINFO_SYMTAB`
  (libsa `LOAD_SYM`) and registered with `ksyms_addsyms_elf` in
  `cpu_startup()` *after* `pmap_map_kernel_tail` installs PTEs for
  the post-`_end` symtab region. Deferred from `pmap_bootstrap`
  because `pmap_kenter_pa` reloads the same pinned scratch slot the
  early UART uses.

## Sources to update when status changes

When you change kernel behavior, update
`doc/system/netbsd/porting-status.md` (the source of truth for
status). Do **not** also maintain a parallel checklist in this file
or in the root CLAUDE.md — that's how the three documents drift apart.
