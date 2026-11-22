# Penumbra -- TODO

Outstanding work and roadmap items, split by area. Each file holds
open entries plus the findings that motivate them.

| File | Covers |
|------|--------|
| [hw-core.md](hw-core.md) | CPU cores and cross-generation ISA work, bus, caches, SDRAM path, fmax and placement |
| [hw-penumbra2.md](hw-penumbra2.md) | gen2 / gen2.5-specific microarchitecture |
| [hw-devices.md](hw-devices.md) | Devices and their programming interfaces, on any board |
| [hw-build.md](hw-build.md) | Build system and test matrix, shared by every board |
| [hw-ulx3s.md](hw-ulx3s.md) | Work bound to the ULX3S board |
| [hw-nexysvideo.md](hw-nexysvideo.md) | Work bound to the Nexys Video board |
| [rom.md](rom.md) | Boot ROM: bring-up, autoconfig, monitor, loading the first program |
| [compiler.md](compiler.md) | LLVM backend: ranked levers and the per-area files below |
| [compiler-selection.md](compiler-selection.md) | Compares, flags, booleans, extensions |
| [compiler-addressing.md](compiler-addressing.md) | Global addresses, PIC, constants, rematerialization |
| [compiler-arith.md](compiler-arith.md) | Multiply, divide, strength reduction |
| [compiler-pipeline.md](compiler-pipeline.md) | Passes, frame and call lowering, legalization, per-generation tuning |
| [kernel.md](kernel.md) | NetBSD MD kernel, drivers, networking |
| [userland.md](userland.md) | libc and other userland |
| [workloads.md](workloads.md) | Profiles and code reviews of real workloads (Doom, Dhrystone, SQLite, audio mixing, CoreMark-Pro) |

An entry lives in the file of the subsystem that owns the fix. A
workload profile whose levers span several subsystems lives in
`workloads.md`, and the per-subsystem entries link to it.

Work bound to one board lives in that board's file. An item that
also creates a programming interface (a device class, a register-map
change) lives with the interface in `hw-devices.md`, since the
interface is written first, and names the boards it serves.

## Closing an entry

A resolved entry is deleted, not marked RESOLVED. Before deleting it,
move what stays useful to where it belongs:

- the reasoning behind the fix → a comment at the fix;
- what changed and what it measured → the commit message;
- protection against a return → a regression test;
- a lesson that generalizes beyond the fix → the subsystem's doc
  under `doc/internals/` (hardware), `doc/llvm/` (compiler) or
  `doc/system/`.

Partly done entries keep only the open remainder, with one line
naming the commit that landed the rest.

## Ruled out

An approach that was tried or analysed and declined stays recorded,
so it is not re-proposed: a short entry (what, the measured result,
why declined, a commit or doc link) under a `## Ruled out` section at
the end of the owning topic file.
