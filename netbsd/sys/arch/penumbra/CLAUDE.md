# Penumbra NetBSD Port — Claude Code Context

This file provides context for the NetBSD machine-dependent port under `netbsd/sys/arch/penumbra/`. The root `CLAUDE.md` has project-wide conventions; `doc/boot/boot-process.md` has the full boot chain design.

## Overview

This is the machine-dependent ("MD") layer for porting NetBSD to Penumbra. It lives inside the NetBSD source subtree (`netbsd/`) which was added as a squashed git subtree from the `netbsd-10` branch of `https://github.com/NetBSD/src.git`.

Penumbra is ILP32, little-endian, 32-bit physical and virtual addresses, 4 KB pages. These properties are the same as MIPS o32 in little-endian mode, so the mips/evbmips port is the primary reference.

## Directory Layout

```
sys/arch/penumbra/
├── include/           # <machine/*.h> headers (16 files)
├── stand/
│   ├── boot/          # Stage 1 bootloader (planned)
│   └── libsa/         # Machine-dependent libsa glue (planned)
```

## Build

### Via build.sh (preferred)

Penumbra is registered in `build.sh` with `TOOLCHAIN_MISSING=yes` (no in-tree GCC). Uses `EXTERNAL_TOOLCHAIN` pointing at our LLVM build, with prefixed symlinks created by `toolchain-setup.sh`.
```sh
sh netbsd/sys/arch/penumbra/toolchain-setup.sh   # one-time: creates penumbra-unknown-none-* symlinks
cd netbsd
./build.sh -U -j4 -m penumbra tools \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest
../build/netbsd-tools/bin/nbmake-penumbra -C sys/arch/penumbra/stand/boot obj  # once: create obj dir
../build/netbsd-tools/bin/nbmake-penumbra -C sys/arch/penumbra/stand/boot      # build
```

**Important:** The `obj` step creates
`build/netbsd-obj/sys/arch/penumbra/stand/boot/` so that build
artifacts go there instead of polluting the source tree.
Without it, bmake silently falls back to in-source builds.
You only need to run `obj` once per subdirectory.

### Standalone (quick iteration, builds in-source)

```sh
make -C netbsd/sys/arch/penumbra/stand/boot -f Makefile.standalone
```

### Header-only compile test

```sh
ln -sfn arch/penumbra/include netbsd/sys/machine
build/llvm/bin/clang --target=penumbra-unknown-none -ffreestanding -nostdinc \
  -D_STANDALONE -I netbsd/sys -c test.c -o /dev/null
rm netbsd/sys/machine
```

## Machine Headers (`include/`)

All 18 headers are minimal stubs, sufficient for `#include <lib/libsa/stand.h>` and `<lib/libsa/loadfile.h>` to compile cleanly. Most integer-type headers delegate to `sys/common_*` via compiler builtins.

| Header | Purpose | Notes |
|--------|---------|-------|
| `int_types.h` | Exact-width integer types | Delegates to `common_int_types.h` (compiler builtins) |
| `int_mwgwtypes.h` | Min-width / greatest-width types | Delegates to `common_int_mwgwtypes.h` |
| `int_const.h` | Integer constant macros | Delegates to `common_int_const.h` |
| `int_limits.h` | Integer limit macros | Delegates to `common_int_limits.h` |
| `int_fmtio.h` | printf/scanf format macros | Delegates to `common_int_fmtio.h` |
| `types.h` | Machine-dependent types | ILP32: 32-bit vaddr/paddr/register, `label_t` for setjmp |
| `param.h` | Machine parameters | MACHINE="penumbra", 4 KB pages, MAXPHYS=64K |
| `cdefs.h` | Compiler definitions | `__ALIGNBYTES=3` (4-byte alignment) |
| `limits.h` | C type limits | ILP32 values (LONG_MAX=0x7fffffff, etc.) |
| `ansi.h` | ANSI C type mappings | Delegates to `common_ansi.h` |
| `endian.h` | Byte order | Includes `sys/endian.h` |
| `endian_machdep.h` | Byte order definition | `_BYTE_ORDER = _LITTLE_ENDIAN` |
| `bswap.h` | Byte-swap macros | `__BSWAP_RENAME` + `sys/bswap.h` |
| `wchar_limits.h` | Wide character limits | 32-bit signed wchar_t/wint_t |
| `signal.h` | Signal definitions | `sig_atomic_t`, minimal `sigcontext` |
| `mcontext.h` | Machine context | 18 gregs (R0-R15 + SR + PC), `_UC_MACHINE_*` macros |
| `elf_machdep.h` | ELF machine type | EM_PENUMBRA (0xF0DA), 7 relocation types |
| `loadfile_machdep.h` | ELF loader macros | BOOT_ELF32, LOAD/READ/BCOPY/BZERO macros for libsa loadfile |

## Key Design Decisions

- **Compiler builtins:** Clang provides `__UINTPTR_TYPE__`, `__SIG_ATOMIC_MAX__`, `__INTMAX_C_SUFFIX__`, `__INTPTR_FMTd__`, etc. This lets us delegate most integer-type headers to `sys/common_*` one-liners instead of manual definitions.
- **Reference port:** evbmips/mips is the primary reference (ILP32, LE, software-managed TLB). However, we don't include `<mips/*.h>` — all headers are self-contained.
- **`machine/` symlink:** NetBSD resolves `<machine/foo.h>` via a symlink at `sys/machine → sys/arch/<port>/include`. This must be created before building.

## Current Status

- [x] Machine headers — 20 headers (added `aout_machdep.h`,
  `mutex.h`), compiles `<lib/libsa/stand.h>` and
  `<lib/libsa/loadfile.h>` cleanly
- [x] libsa glue — `sdblk.c` (SD block device strategy via SPI),
  `cons.c` (UART console)
- [x] Build system — standalone Makefile + build.sh integration
  via `bsd.prog.mk`
- [ ] Boot loader (`PENBOOT.ELF`) — PIE ELF loaded by ROM from
  FAT32, self-relocates at startup. Responsible for ELF kernel
  loading, MMU enable, bootinfo translation, jump to kernel
- [ ] Kernel port — locore.S, pmap, trap handling

**Note:** The original stage 1/stage 2 bootloader split via
libsa has been superseded.  The ROM now includes its own FAT32
reader and ELF loader, and loads `PENBOOT.ELF` directly from
the boot partition into dynamically allocated RAM.
The libsa-based `boot.c`/`conf.c`/`sdblk.c` code in `stand/boot/`
is retained for reference but is no longer the active boot path.
The new boot loader will be built independently of libsa.

## Known Issues

_(s1 store bug fixed — widenScalarToNextPow2 +
lowerIfMemSizeNotByteSizePow2 added to legalizer)_

## Next Steps

1. **Boot loader** — write `PENBOOT.ELF`: CRT self-relocator,
   parse boot data, load kernel ELF from FAT32, enable MMU,
   build NetBSD bootinfo, jump to kernel
2. **PIC jump table optimization** — narrow 32-bit
   label-difference entries to 16-bit when offsets fit ±32KB
   (EK_Inline + LDH)
