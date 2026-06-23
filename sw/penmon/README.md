# penmon — Penumbra hardware-counter system monitor

A `top`/`htop`/`systat vm`-style live dashboard that pairs the usual OS
metrics (CPU busy %, memory, load, per-process CPU) with Penumbra's
**hardware performance counters** — CPI, MIPS, and L1I/L1D/L2 cache hit
rates — read straight from the sysctl tree.
```
 PENUMBRA penmon                         up 0:03:42  load 0.42  25.0 MHz
 CPU ████████░░░░░░░░░░░░░░░░░░░░  us  38% sy  12% in   2% id  48%
 CPI  4.12 ███████░░░░░░░░░░░░░░   MIPS   6.07   insn/s 6071000
 ───────────────────────────────────────────────────────────────────
 CACHE      hit%        (hit bar)      miss/s  history
 L1I  98.1% ███████████████████░    1200/s  ▁▂▁▃▂▁▁▂
 L1D  94.2% ██████████████████░░   12000/s  ▂▃▅▃▂▂▃▄
 L2   71.4% ██████████████░░░░░░    8300/s  ▅▅▆▇▆▅▅▆
 ───────────────────────────────────────────────────────────────────
 MEM  ███████░░░░░░░░░░░░░░░░░░░░   24.3M / 64.0M used  (39.7M free)
 ───────────────────────────────────────────────────────────────────
    PID USER        %CPU      RSS ST COMMAND
    234 root        12.3     2.1M R  sh
      1 root         0.1     120K S  init
```

## Metrics

- **CPU bar** — `kern.cp_time` deltas: green user, cyan system, red interrupt,
  dim idle. Shows *how much* the CPU ran.
- **CPI gauge** — `d(machdep.cpu.cycles) / d(machdep.cpu.insns_retired)`.
  Shows *how well* it ran. The scale is **relative to the machine's own
  best-observed CPI** (auto-discovered at runtime), so it works unchanged
  across the microcoded gen1 (floor ~3) and a future pipelined gen2
  (floor ~1): empty bar = running at this CPU's best, filling = stalling.
- **Cache rows** — `machdep.cache.{l1i,l1d,l2}.*`: hit %, miss rate, and a
  rolling sparkline of recent hit rate.
- **MEM** — `vm.uvmexp2` used/total.
- **Process table** — `KERN_PROC2`, sorted by %CPU.

All hardware counters are free-running 32-bit and wrap in ~85–170 s at
25 MHz, so penmon only ever shows *rates over the sample interval*, never
raw totals (`counter_delta()` in `sample.c` corrects for wrap).

## Build

Cross-compiles with the in-tree LLVM toolchain against the NetBSD sysroot:

```sh
cd sw/penmon
make            # build/penmon/penmon          (dynamic)
make static     # build/penmon/penmon-static   (static — no libs needed on target)
```

Overrides: `LLVM_PREFIX=`, `DESTDIR=`, `COPT=`.

## Kernel dependency

The CPI/MIPS panel needs the **`machdep.cpu.*` sysctl leaves** added by
`netbsd/sys/arch/penumbra/penumbra/cpu_perfctrs.c`. Rebuild the kernel
(`build.sh kernel=MINIMAL`) to pick them up. penmon degrades gracefully on
an older kernel: missing sysctls read as zero, so the cache and process
panels still work and only CPI/MIPS show 0.

## Deploy to a rootfs image

penmon is wired into the build. `make sdimage-rootfs` runs `make netbsd-overlay`,
which builds every custom utility and stages it into a shared overlay
fake-root (`build/netbsd-overlay`); `mkrootfs.sh -O` then copies the whole
tree into the image. penmon's `overlay` target installs just the static
binary at `/usr/local/bin/penmon`. The renderer emits ANSI directly (no
libcurses, no terminfo database), so the binary is self-contained — it needs
no base-system files beyond what every rootfs already carries.

```sh
make sdimage-rootfs                 # boot + rootfs, with penmon + demos

make netbsd-overlay                 # just stage the tree (to inspect it)
find build/netbsd-overlay -type f
```

To add another custom utility to the image: give its Makefile an `overlay`
target that installs into `$(OVERLAY_ROOT)`, then add it to `NETBSD_OVERLAYS`
in the top-level Makefile.

- **Terminal:** any UTF-8, ANSI-colour terminal (the common case over the
  serial console). `TERM` is not consulted — penmon emits ANSI escapes
  directly rather than going through terminfo.

## Run

```sh
penmon          # 1 s refresh
penmon -d 0.5   # faster
```

Keys: `q` quit · `space` force refresh · `+`/`-` change interval.

## Display notes

penmon does its own drawing through a small virtual-screen layer
(`screen.c`): the renderer redraws the whole frame into an off-screen cell
grid each tick, and a damage diff emits only the changed cells as one ANSI
byte stream per `write(2)` — keeping curses' cheap half (the virtual-screen
diff) while dropping its expensive half (libcurses flushes once per
character over the serial console). Bars and rules use Unicode block/line
glyphs (`█ ░ ─`) and sparklines a block-height ramp (`▁▂▃▄▅▆▇█`), emitted as
raw UTF-8 — so the look depends only on a UTF-8 *host* terminal, not on the
target's locale. Each sparkline takes a single colour (from its most recent
sample) so the strip costs one colour run on the wire.
