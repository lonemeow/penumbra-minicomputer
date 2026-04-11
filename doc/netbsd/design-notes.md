# NetBSD Port -- Design Notes

Architectural decisions and rationale for the Penumbra NetBSD port.
For current status, see `porting-status.md`.

## Reference Port

evbmips (ILP32 little-endian MIPS with software-managed TLB).
Shares key constraints: no hardware page table walker, software
TLB refill, no direct-mapped memory segments.  All code is
Penumbra-specific -- no `<mips/*.h>` includes.

## Machine Properties

- ILP32, little-endian, 4 KB pages
- 16 registers: R0=zero, R12=TP, R13=LR, R14=SP, R15=PC
- 2G/2G user/kernel VA split (kernel text at `0x8001_0000`)
- Software-managed TLB: 64-entry 2-way SA + 4-entry FA pinned
- Split I/D PIPT caches, write-through D-cache
- No FPU -- all floating-point via soft-float

## Virtual Memory Layout

```
0x0000_0000              unmapped null guard page
0x0000_1000              user text / data / bss
                         heap (grows up)
0x0400_0000  USRSTACK    stack top at 64 MB (grows down)
0x8000_0000              kernel VA start
0x8001_0000              kernel text (KERNEL_TEXT_BASE)
                         kernel data / bss / page tables
                         MMIO devices (pmap_map_device)
                         virtual_avail -> kernel VM pool (UVM)
0xFFFF_B000  VECTOR_VA    pinned slot 0: vector page
0xFFFF_C000  SCRATCH_VA   pinned slot 3: scratch window
0xFFFF_D000  PT_L2WIN_VA  pinned slot 2: L2 window (handler)
0xFFFF_E000  PT_L1_VA     pinned slot 1: current L1 table
0xFFFF_F000              unmapped guard
```

## Page Table Design

Always 2-level: L1 (1024 entries, 4 KB) --> L2 (1024 entries,
4 KB, covers 4 MB each).  No direct-map -- differs from MIPS
KSEG0/KSEG1, but matches the 74xx-feasible TLB design where
all memory access goes through the TLB.

The TLB miss handler is context-blind: it walks the 2-level
table via the pinned L1 (slot 1) and a transient L2 window
(slot 2).  No stack, no curproc, no metadata access needed.
This keeps the handler stateless and fast.

Physical pages without kernel VAs are accessed via the scratch
window (pinned slot 3, `SCRATCH_VA`).

### Own pmap vs MI pmap

Penumbra uses its own `pmap.c`, not the shared MI pmap from
`sys/uvm/pmap/pmap.c`.  The MI pmap is used by MIPS, RISC-V,
and PowerPC -- architectures that opted into a common
implementation with MD hooks (`pmap_md_page_syncicache`, etc.).

Reasons for a standalone pmap:

- The TLB refill handler has Penumbra-specific constraints
  (pinned slot window, 2-level walk, no KSEG0).
- Simpler to understand and debug during bring-up.
- The pinned scratch window pattern doesn't fit the MI pmap's
  assumptions about direct-mapped kernel memory.

The standalone pmap implements the full NetBSD pmap interface
directly: `pmap_enter`, `pmap_create`/`pmap_destroy`,
`pmap_activate`, `pmap_protect`/`pmap_remove`/`pmap_remove_all`,
`pmap_page_protect`, PV lists, ASID management, etc.

## ASID Management

Generational allocator: each pmap has an ASID (1--255) tagged
with a generation number.  On `pmap_activate`, a stale generation
triggers re-allocation.  When all 255 IDs are exhausted, the
generation bumps, the TLB is flushed, and allocation restarts
from 1.  Kernel always uses ASID 0 with G=1 (global) entries.

## I-Cache Coherency

Split I/D caches are not hardware-coherent.  After mapping
executable pages (demand paging, exec), data goes through
D-cache --> memory, but I-cache may hold stale lines.

Three invalidation points:
1. `pmap_enter()` flushes I-cache when `prot & VM_PROT_EXECUTE`
2. `pmap_procwr()` flushes for ptrace/exec code writes (MI hook)
3. Boot-time `wrsys` in locore.S (after vector page setup)

Hardware currently supports only full I-cache invalidation
(write to `SYSDEV_ICACHE` / `CACHE_INVAL`).  Write-through
D-cache means no writeback is needed before the I-cache flush.
Per-address invalidation can be added to the hardware later
without changing the hook placement.

## Trap Handler: Volatile State Stash

The trap handler has a subtle constraint: TLB misses during
trapframe allocation on the kernel stack could clobber ESR,
EPC, FAULT_ADDR, and FAULT_STATUS (hardware registers latched
on exception).  Solution: `_trap_common` (locore.S) copies these
into the pinned vector page's scratch area before any faultable
stack access.  The C `trap()` function reads from scratch, not
the hardware registers.

## Boot Stack Switch

ARM-style pattern: `penumbra_init()` runs on a 4 KB boot stack
(allocated in BSS by locore.S) and returns the new SP (lwp0's
12 KB USPACE kernel stack).  locore.S switches SP before calling
`penumbra_main()` --> `main()`.  The 4 KB boot stack overflows
at `-O0` + DIAGNOSTIC if used for `main()`.

## UART Mapping Timing

`pmap_map_device()` for the console UART must run before
`uvm_pageboot_alloc()`, because the latter snapshots
`virtual_avail` into UVM via `pmap_virtual_space()`.  After
that, pmap's local `virtual_avail` is stale.  The UART starts
life via the TLB scratch window (early boot), then gets a
permanent mapping via `pmap_map_device()` in `pmap_bootstrap()`.

## Syscall Convention

R11 (scratch register) carries the syscall number, not R1.
This preserves R1 as the first C argument, matching the ABI.
The SYSTRAP macro in `SYS.h` loads R11 and executes `SYSCALL`.

Return: R1=rval[0], R2=rval[1], carry flag = error indicator.
The carry-flag convention (like ARM/aarch64) lets userland
distinguish error returns from valid negative values without
ambiguity.  Two-value return (R1+R2) is needed by fork (child
gets R1=0, R2=1) and pipe (two fds).

## Atomics

- **Userland:** RAS (Restartable Atomic Sequences).
  `atomic_cas_32` uses plain load/cmp/store bracketed by RAS
  labels; `.init_array` constructor registers the sequence via
  `rasctl()`.  `userret()` checks `ras_lookup()` on every
  return to user mode.
- **Kernel:** interrupt-disable CAS (`RDSPR SR` / `DI` / op /
  `WRSPR SR`).  Safe on uniprocessor.

Generic CAS-based inc/dec/add/and/or built on top.  No-op
memory barriers (uniprocessor, no store buffer reordering).

## Userland Build Integration

Uses stock `toolchains::NetBSD` in clang with Penumbra
emulation flags in `NetBSD.cpp` (no custom toolchain class for
NetBSD target).  `PenumbraToolChain` retained for bare-metal
(`penumbra-unknown-none`) only.

Key build flags: `MKSOFTFLOAT=yes`, `MKCXX=no`, `HAVE_SSP=no`,
`HAVE_LIBGCC_EH=yes` (skip libunwind), `SLOPPY_FLIST=yes`
(tolerate missing `ld.elf_so`).

## NetBSD Tree Modifications

Files modified outside `sys/arch/penumbra/`:

**Build system:**
- `build.sh` -- penumbra in `valid_MACHINE_ARCH`
- `share/mk/bsd.own.mk` -- TOOLCHAIN_MISSING, HAVE_LLVM,
  MACHINE_GNU_PLATFORM, HAVE_SSP, jemalloc LG_QUANTUM
- `share/mk/bsd.endian.mk` -- penumbra in little-endian list
- `tools/Makefile`, `tools/headerlist` -- tool build integration

**Common libraries:**
- `common/lib/libc/arch/penumbra/string/` -- assembly memcpy,
  memset, memcmp, strlen, strcmp, strcpy (shared kernel+userland)
- `common/lib/libc/arch/penumbra/atomic/` -- CAS, RAS init,
  generic + 64-bit atomic ops

**Userland libraries:**
- `lib/csu/arch/penumbra/` -- C startup (crt0, crti, crtn,
  crtbegin, crtend)
- `lib/libc/arch/penumbra/` -- SYS.h, cerror, syscall wrappers,
  setjmp, softfloat, gdtoa, makecontext, mulsi3
- `lib/libpthread/arch/penumbra/pthread_md.h` -- minimal stubs
- `lib/libkvm/kvm_penumbra.c` -- kvm support

**Other:**
- `etc/etc.penumbra/` -- MAKEDEV.conf, Makefile.inc, ttys
- `crypto/external/bsd/openssl/lib/libcrypto/arch/penumbra/` --
  ec.inc (no 64-bit NIST curves), sha.inc
- `external/bsd/jemalloc/` -- LG_QUANTUM=3 type header
- `sys/kern/init_main.c` -- minor modification
- `sys/lib/libkern/arch/penumbra/Makefile.inc`
- `usr.bin/xlint/arch/penumbra/targparam.h`
