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

```sh
# Generate kernel Makefile from config
cd netbsd/sys/arch/penumbra/conf
../../../../../build/netbsd-tools/bin/nbconfig \
  -b ../../../../../build/netbsd-kernel/MINIMAL \
  -s ../../../../../netbsd/sys MINIMAL

# Build (from the compile directory)
cd ../../../../../build/netbsd-kernel/MINIMAL
../../../build/netbsd-tools/bin/nbmake-penumbra depend
../../../build/netbsd-tools/bin/nbmake-penumbra
```

Or more concisely from the project root:
```sh
# config
(cd netbsd/sys/arch/penumbra/conf && \
  $PWD/../../../../../build/netbsd-tools/bin/nbconfig \
  -b $PWD/../../../../../build/netbsd-kernel/MINIMAL \
  -s $PWD/../../../.. MINIMAL)

# depend + build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL
```

**Important:** Re-run `nbconfig` after changing any `conf/` files.
The `build/netbsd-kernel/` directory is gitignored.

### Bootloader Build (standalone)

```sh
make -C netbsd/sys/arch/penumbra/stand/boot -f Makefile.standalone
```

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
                         kernel data / bss
                         kernel VM (dynamic mappings)
0xFF00_0000              device MMIO region
0xFFFF_0000              ROM (identity-mapped)
0xFFFF_FFFF              end
```

**Page table optimization:** Processes start with a flat single-level
page table covering 0–64 MB (16K entries, 64 KB). If VA usage grows
beyond `PENUMBRA_PT1_LIMIT`, pmap promotes to a 2-level table.

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
| `locore.S` | Entry point, exception vectors, trap frame save/restore, setjmp/longjmp |
| `machdep.c` | `penumbra_init()`, `cpu_startup()`, `cpu_reboot()`, LWP stubs |
| `autoconf.c` | `cpu_configure()`, `cpu_rootconf()` |
| `mainbus.c` | Root bus device driver |
| `cpu.c` | CPU device driver |
| `trap.c` | Exception dispatch, SPL stubs |
| `pmap.c` | Software TLB management (stub — flat/2-level PT design) |
| `genassym.cf` | Struct offset definitions for assembly code |

## Current Status

- [x] Machine headers — 39 files, sufficient for kernel compilation
- [x] Kernel config — `config MINIMAL` generates Makefile successfully
- [x] `make depend` — passes cleanly
- [x] `make` — begins compiling kernel code
- [x] libsa glue — `sdblk.c` (SD block device), `cons.c` (UART)
- [x] Build system — build.sh integration, out-of-tree kernel build
- [ ] **LLVM codegen gaps** — `G_PTRTOINT` to s8 not legalized
  (first file to fail: `prop_data.c`). More patterns will surface.
- [ ] Boot loader (`PENBOOT.ELF`) — CRT self-relocator done,
  kernel loading / MMU enable not yet implemented
- [ ] Kernel port — all MD stubs need real implementations

## Known Issues

- `G_PTRTOINT` to sub-word types (s8/s16) crashes the LLVM backend.
  Needs widening rule in the legalizer.
- _(s1 store bug fixed — widenScalarToNextPow2 +
  lowerIfMemSizeNotByteSizePow2 added to legalizer)_

## Next Steps

1. **LLVM codegen hardening** — fix `G_PTRTOINT`/`G_INTTOPTR`
   legalization for sub-word types; more patterns will surface
   as kernel compilation progresses
2. **Boot loader** — kernel ELF loading, MMU enable, bootinfo
   translation, jump to kernel
3. **Kernel implementation** — fill in pmap, trap handling,
   console driver, get to `main()` → `cpu_startup()`
