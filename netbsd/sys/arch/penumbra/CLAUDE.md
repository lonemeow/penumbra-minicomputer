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

The port is compiled using the Penumbra clang cross-compiler (`build/llvm/bin/clang --target=penumbra-unknown-none`). The `machine/` include path is resolved by creating a symlink: `netbsd/sys/machine → arch/penumbra/include`.

Test that headers compile:
```sh
ln -sfn arch/penumbra/include netbsd/sys/machine
build/llvm/bin/clang --target=penumbra-unknown-none -ffreestanding -nostdinc \
  -D_STANDALONE -I netbsd/sys -c test.c -o /dev/null
rm netbsd/sys/machine
```

## Machine Headers (`include/`)

All 16 headers are minimal stubs, sufficient for `#include <lib/libsa/stand.h>` to compile cleanly. Most integer-type headers delegate to `sys/common_*` via compiler builtins.

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

## Key Design Decisions

- **Compiler builtins:** Clang provides `__UINTPTR_TYPE__`, `__SIG_ATOMIC_MAX__`, `__INTMAX_C_SUFFIX__`, `__INTPTR_FMTd__`, etc. This lets us delegate most integer-type headers to `sys/common_*` one-liners instead of manual definitions.
- **Reference port:** evbmips/mips is the primary reference (ILP32, LE, software-managed TLB). However, we don't include `<mips/*.h>` — all headers are self-contained.
- **`machine/` symlink:** NetBSD resolves `<machine/foo.h>` via a symlink at `sys/machine → sys/arch/<port>/include`. This must be created before building.

## Current Status

- [x] Machine headers — compiles `<lib/libsa/stand.h>` cleanly
- [ ] libsa glue — block device strategy (SPI/SD sector reads)
- [ ] Stage 1 bootloader — main, conf.c, devopen
- [ ] Stage 2 bootloader — ELF loading, MMU enable
- [ ] Kernel port — locore.S, pmap, trap handling

## Next Steps

1. **libsa block device** — implement `blkdevstrategy()` that reads SD sectors via SPI controller, bridging `hw/rom/sdcard.c` protocol to libsa's `open_file` interface
2. **Stage 1 boot main** — `conf.c` (wire dosfs + block device), `devopen()` (parse boot data, find boot partition), `boot.c` (load stage 2 from FAT32)
3. **Build integration** — standalone Makefile (like `hw/rom/Makefile`) using clang pipeline; eventually wire into `make simulate`
