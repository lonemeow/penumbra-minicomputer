# Penumbra Minicomputer — Build System
# Usage:
#   make simulate            — build ROM + ISS, run interactively (fast)
#   make simulate-rtl        — build ROM + Verilator RTL sim (cycle-accurate)
#   make test-iss            — run all HW tests on ISS (fast, no Docker)
#   make test                — run all HW tests on RTL sim (slow, Docker)
#   make sim MOD=<name>      — build & run testbench for a module
#   make smoke               — toolchain smoke test
#   make wave MOD=<name>     — open waveform in GTKWave
#   make sdimage             — build SD image with bootloader, kernel, rootfs
#   make fpga-lint [TOP=<m>] — Verilator lint check on FPGA modules
#   make fpga TOP=<module> — synthesize + PnR + bitstream
#   make flash [TOP=<module>]— fpga + flash to ULX3S via USB
#   make clean               — remove build artifacts

# ── Configuration ──────────────────────────────────────────────
# Docker-based Verilator (avoids host install, works on WSL2)
DOCKER_IMAGE ?= verilator/verilator:latest
DOCKER_RUN   = docker run --rm -u $(shell id -u):$(shell id -g) -v $(CURDIR):/work -w /work

# Verilator runs inside the container; its entrypoint IS verilator.
# For commands that aren't verilator (like running the built binary),
# we override the entrypoint.
#
# OPT_BUILD overrides the host C++ optimization Verilator uses for
# generated simulation code.  Verilator's default is -Os (size); we
# default to -O2 for ~2-3x simulation runtime at modest build-time
# cost.  Long-running sims (NetBSD boot, etc.) benefit further from
# OPT_BUILD="-O3 -flto -march=native" — slower to build, faster to
# run.  Set OPT_BUILD="-Os" to restore the original size-optimized
# build when iterating on testbenches.
OPT_BUILD ?= -O2

VERILATOR_FLAGS = --cc --exe --build -Wall --assert \
                  $(if $(VCD),--trace) \
                  -CFLAGS "-std=c++17 $(OPT_BUILD)" \
                  -Ihw/rtl/common -Ihw/rtl/penumbra1 -Ihw/rtl/penumbra2 -Ihw/rtl/bus -Ihw/rtl/mmu -Ihw/rtl/io -Ihw/rtl/io/sdram -Ihw/rtl/soc -Ihw/rtl/sim

BUILD_DIR   = build
WAVE_DIR    = waves

# ── Generic module simulation ──────────────────────────────────
# Usage: make sim MOD=alu  (expects hw/rtl/**/alu.sv and hw/sim/tb_alu.cpp)
#        make sim MOD=machine_sim PROG=test_mem TB=tb_cpu_mem
MOD  ?=
PROG ?= test_add
TB   ?= tb_$(MOD)

# Shared packages — always included. --top-module tells Verilator which
# module is the DUT (otherwise it picks the first file = a package).
# Add new packages here as the design grows.
PKG_SV = hw/rtl/common/penumbra_pkg.sv hw/rtl/penumbra2/penumbra2_pkg.sv hw/rtl/io/sdram/sdram_pkg.sv

# ── Assembler tools ──────────────────────────────────────────
PASM  = python3 sw/tools/pasm.py
UASM  = python3 hw/tools/uasm.py

# ── LLVM toolchain (for C boot ROM) ─────────────────────────
# Override with: make simulate LLVM_PREFIX=/path/to/llvm-build
LLVM_PREFIX ?= $(CURDIR)/build/llvm
CC        = $(LLVM_PREFIX)/bin/clang --target=penumbra-unknown-none
MC        = $(LLVM_PREFIX)/bin/llvm-mc -triple=penumbra
LD        = $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY   = $(LLVM_PREFIX)/bin/llvm-objcopy
BIN2HEX   = python3 sw/tools/bin2hex.py

.PHONY: sim
sim:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make sim MOD=alu)
endif
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module $(MOD) \
		--Mdir $(BUILD_DIR)/$(MOD).verilator \
		-o ../V$(MOD) \
		$(PKG_SV) $$(find hw/rtl -name '$(MOD).sv') hw/sim/$(TB).cpp
	@# Assemble program (per-program hex under build/hex/, passed to the
	@# RTL via +rom_hex= — the root program.hex belongs to the boot ROM)
	@# and microcode for $readmemh
	@mkdir -p $(BUILD_DIR)/hex
	@if test -n "$(PROG)"; then \
		if test -f hw/sim/programs/$(PROG).s; then \
			$(PASM) --org 0xFFFF0000 hw/sim/programs/$(PROG).s -o $(BUILD_DIR)/hex/$(PROG).hex; \
		else echo "ERROR: hw/sim/programs/$(PROG).s not found" >&2; exit 1; fi; \
	fi
	@if test -f hw/microcode/microcode.uasm; then $(UASM) hw/microcode/microcode.uasm -o microcode.hex; fi
	@echo "── Running $(MOD) testbench ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(MOD) $(DOCKER_IMAGE) $(if $(PROG),+rom_hex=$(BUILD_DIR)/hex/$(PROG).hex)

.PHONY: wave
wave:
ifndef MOD
	$(error Set MOD=<module_name>, e.g. make wave MOD=alu)
endif
	gtkwave $(WAVE_DIR)/$(MOD).vcd &

# ── Run all program tests ──────────────────────────────────────
# Discovers all hw/sim/programs/test_*.s files, runs each through
# tb_cpu_prog on machine_sim, reports pass/fail summary.
TEST_PROGS := $(sort $(basename $(notdir $(wildcard hw/sim/programs/test_*.s))))

.PHONY: test
test:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@# Build machine_sim + tb_cpu_prog once (reuse for all programs)
	@$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim.verilator \
		-o ../Vmachine_sim \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_cpu_prog.cpp
	@# Assemble microcode once (shared by all programs)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@mkdir -p $(BUILD_DIR)/hex
	@pass=0; fail=0; failed=""; \
	for prog in $(TEST_PROGS); do \
		if ! $(PASM) --org 0xFFFF0000 hw/sim/programs/$$prog.s -o $(BUILD_DIR)/hex/$$prog.hex; then \
			printf "  \033[31mFAIL\033[0m  %s (assembler error)\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
			continue; \
		fi; \
		if $(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vmachine_sim $(DOCKER_IMAGE) \
			+rom_hex=$(BUILD_DIR)/hex/$$prog.hex > /dev/null 2>&1; then \
			printf "  \033[32mPASS\033[0m  %s\n" "$$prog"; \
			pass=$$((pass + 1)); \
		else \
			printf "  \033[31mFAIL\033[0m  %s\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
		fi; \
	done; \
	echo ""; \
	total=$$((pass + fail)); \
	echo "$$pass/$$total tests passed"; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

# ── Run all hardware module unit tests ────────────────────────
# Each entry is "MOD" when the testbench filename is the default
# `tb_<MOD>.cpp` and the top module is `<MOD>`.  Use "MOD:TB" when
# the wrapper module name diverges from the testbench filename
# (typically because a `<thing>_test.sv` wrapper drives a smaller
# RTL module — see hw/rtl/sim/).
#
# Integration testbenches (tb_cpu_prog, tb_cpu_top, tb_cpu_mem,
# tb_interactive) are not listed here — they are covered by
# `make test`, `make simulate`, etc.
#
# Usage: make test-modules
MODULE_TESTS = \
    alu \
    amux \
    bmux \
    byte_ext \
    byte_rep \
    cond_eval \
    datapath \
    divmul \
    field_ext \
    imm_ext \
    mar \
    mdr \
    pc_mux \
    pc_reg \
    regfile \
    status_reg \
    wmux \
    penumbra2_regfile \
    penumbra2_scoreboard \
    penumbra2_alu \
    penumbra2_regmap \
    penumbra2_decode \
    penumbra2_flag_bypass \
    penumbra2_id_stage \
    penumbra2_ex_stage \
    penumbra2_mem_stage \
    penumbra2_wb_stage \
    penumbra2_spr_file \
    penumbra2_vecfetch \
    penumbra2_irq \
    penumbra2_spine \
    unified_mem \
    uart \
    busctl \
    tlb \
    tlb_pinned \
    tlb_unit \
    tlb_bram \
    tlb_unit_bram \
    mmu \
    mmu_bram \
    cpu_bus_arbiter \
    slip_rx \
    slip_tx \
    spi_fifo \
    timer \
    sdram_test \
    sdram_cdc \
    cache_test:tb_cache \
    cache_vipt_test:tb_cache_vipt \
    l2_cache \
    autoconfig_test:tb_autoconfig \
    spi_test:tb_spi \
    sdram_adapter_test:tb_sdram_adapter

.PHONY: test-modules
test-modules:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@pass=0; fail=0; failed=""; \
	for entry in $(MODULE_TESTS); do \
		mod=$${entry%%:*}; \
		tb=$${entry#*:}; \
		[ "$$tb" = "$$entry" ] && tb=tb_$$mod; \
		if $(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
			--top-module $$mod \
			--Mdir $(BUILD_DIR)/$$mod.verilator \
			-o ../V$$mod \
			$(PKG_SV) $$(find hw/rtl -name "$$mod.sv") hw/sim/$$tb.cpp \
			>/dev/null 2>&1 && \
		   $(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$$mod $(DOCKER_IMAGE) \
			>/dev/null 2>&1; then \
			printf "  \033[32mPASS\033[0m  %s\n" "$$mod"; \
			pass=$$((pass + 1)); \
		else \
			printf "  \033[31mFAIL\033[0m  %s\n" "$$mod"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$mod"; \
		fi; \
	done; \
	echo ""; \
	total=$$((pass + fail)); \
	echo "$$pass/$$total module tests passed"; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

# ── Aggregate: program tests + module tests ───────────────────
# Usage: make test-all
.PHONY: test-all
test-all: test test-modules

# ── Penumbra/2 core smoke test ────────────────────────────────
# Builds penumbra2_core (fetch + spine) with a program assembled into its
# i-mem and runs it to BREAK. Separate from `test`/`test-modules` because it
# loads an assembled program (which test-modules does not) and targets the
# gen2 core rather than gen1 machine_sim.
# For a different program, use `make sim MOD=penumbra2_core PROG=<prog>
# TB=tb_penumbra2_core` directly.
# Usage: make test-penumbra2
.PHONY: test-penumbra2
test-penumbra2:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_smoke TB=tb_penumbra2_core

# ── Penumbra/2 core branch-redirect test ──────────────────────
# Same core, the branch-redirect milestone program: forward taken/not-taken
# branches, an unconditional branch, and a backward loop. Self-checks into R1,
# so it validates the taken-branch PC redirect + 3-bubble front-end flush.
# Usage: make test-penumbra2-branch
.PHONY: test-penumbra2-branch
test-penumbra2-branch:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_branch TB=tb_penumbra2_branch

# ── Penumbra/2 core load/store test ───────────────────────────
# Same core, the MEM-stage data-path milestone program: word/half/byte
# store-then-load round-trips against the BRAM data memory, sub-word extract,
# and byte_en lane isolation. Self-checks into R1, reusing the branch tb's
# PASS-flag check.
# Usage: make test-penumbra2-loadstore
.PHONY: test-penumbra2-loadstore
test-penumbra2-loadstore:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_loadstore TB=tb_penumbra2_branch

# ── Penumbra/2 core precise-exception store-squash test ───────
# Same core: a younger store in the shadow of an older fault (misaligned load →
# VEC_ALIGN) must have its memory write cancelled by the flush. The handler
# reloads the target and proves a pre-seeded sentinel survived. Validates the
# "store commit vs. fault flush" precise-exception requirement. Self-checks R1.
# Usage: make test-penumbra2-store-squash
.PHONY: test-penumbra2-store-squash
test-penumbra2-store-squash:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_store_squash TB=tb_penumbra2_branch

# ── Penumbra/2 core divmul + memory-op hazard test ────────────
# Same core: a load immediately behind a dual-write divmul must not drop the
# divmul's high-half (Rdh) write. MEM must defer the load's launch while WB
# back-pressures across the divmul's 2-cycle aux write. Self-checks R1.
# Usage: make test-penumbra2-divmul-store
.PHONY: test-penumbra2-divmul-store
test-penumbra2-divmul-store:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_divmul_store TB=tb_penumbra2_branch

# ── Penumbra/2 core RDSYS sysreg-read test ────────────────────
# Same core: RDSYS reads the CPU-internal cpuid/machid identity devices through
# MEM's sysreg sideband (2-cycle access, registered device response). Checks the
# cpuid name registers and that the device selector routes by sys_dev.
# Self-checks R1.
# Usage: make test-penumbra2-sysread
.PHONY: test-penumbra2-sysread
test-penumbra2-sysread:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_sysread TB=tb_penumbra2_branch

# ── Penumbra/2 core WRSYS sysreg-write test ───────────────────
# Same core: WRSYS writes a GPR value to a CPU-internal writable sysreg (the
# scratch device) at the EX drain-commit, then RDSYS reads it back. Round-trips
# two values through two registers. Self-checks R1.
# Usage: make test-penumbra2-syswrite
.PHONY: test-penumbra2-syswrite
test-penumbra2-syswrite:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_syswrite TB=tb_penumbra2_branch

# ── Penumbra/2 core WRSYS context-synchronization test ────────
# Same core: WRSYS is context-synchronizing — it re-fetches its successor after
# commit. Each WRSYS here precedes an increment; exactly-once execution (R5=3)
# proves the re-fetch neither duplicates the held copy nor skips it.
# Usage: make test-penumbra2-resync
.PHONY: test-penumbra2-resync
test-penumbra2-resync:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_resync TB=tb_penumbra2_branch

# ── Penumbra/2 core exception-entry test ──────────────────────
# Same core, the exception-entry milestone program: a misaligned load takes
# VEC_ALIGN, the pipeline flushes + saves state, and the vector-fetch FSM
# redirects to a handler (installed in the RAM vector table at run time). The
# handler sets the PASS flag; the poison between fault and handler must be
# flushed. Self-checks into R1, reusing the branch tb's PASS-flag check.
# Usage: make test-penumbra2-fault
.PHONY: test-penumbra2-fault
test-penumbra2-fault:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_fault TB=tb_penumbra2_branch

# ── Penumbra/2 core exception round-trip test ─────────────────
# Same core, the ERET milestone program: a fault vectors to a handler that
# fixes the cause and ERETs back to EPC, which re-executes and completes.
# Exercises SR<-ESR restore + PC<-EPC redirect. Self-checks into R1.
# Usage: make test-penumbra2-eret
.PHONY: test-penumbra2-eret
test-penumbra2-eret:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_eret TB=tb_penumbra2_branch

# ── Penumbra/2 core software-trap test ────────────────────────
# Same core, the SYSCALL milestone program: SYSCALL raises VEC_SYSCALL at EX
# and vectors to a handler (installed in the RAM table); the poison after it
# is flushed. Self-checks into R1.
# Usage: make test-penumbra2-syscall
.PHONY: test-penumbra2-syscall
test-penumbra2-syscall:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_syscall TB=tb_penumbra2_branch

# ── Penumbra/2 core interrupt test ────────────────────────────
# Same core, the interrupt milestone program: an external IRQ (held by the
# testbench) is masked until EI + the one-instruction shadow pass, then
# recognized at a fetch boundary and vectored (drain-and-take) to a handler.
# Self-checks into R1; the tb (tb_penumbra2_intr) drives the IRQ line.
# Usage: make test-penumbra2-intr
.PHONY: test-penumbra2-intr
test-penumbra2-intr:
	@$(MAKE) sim MOD=penumbra2_core PROG=penumbra2_intr TB=tb_penumbra2_intr

# ── Run all program tests on ISS (fast, no Docker) ────────────
# Same test programs as `make test` but runs on the ISS.
# Usage: make test-iss
.PHONY: test-iss
test-iss: $(ISS)
	@pass=0; fail=0; failed=""; \
	for prog in $(TEST_PROGS); do \
		if ! $(PASM) --org 0xFFFF0000 hw/sim/programs/$$prog.s -o /tmp/$$prog.hex 2>/dev/null; then \
			printf "  \033[31mFAIL\033[0m  %s (assembler error)\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
			continue; \
		fi; \
		r1=$$(echo "break" | timeout 5 ./$(ISS) /tmp/$$prog.hex +halt-on-break +trace=/tmp/iss_test.log 2>/dev/null; \
			tail -1 /tmp/iss_test.log 2>/dev/null | grep -o 'R1=[0-9a-f]*' | head -1); \
		if echo "$$r1" | grep -q '00000001'; then \
			printf "  \033[32mPASS\033[0m  %s\n" "$$prog"; \
			pass=$$((pass + 1)); \
		else \
			printf "  \033[31mFAIL\033[0m  %s ($$r1)\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
		fi; \
	done; \
	echo ""; \
	total=$$((pass + fail)); \
	echo "$$pass/$$total tests passed"; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

# ── Compiler Correctness Tests ─────────────────────────────────
# Runs curated tests from llvm-test-suite on ISS +hosted mode.
# Usage: make test-compiler [OPT="-O2"]
TEST_COMPILER_DIR = test/compiler
HARNESS_DIR      = $(TEST_COMPILER_DIR)/harness
LLVM_TEST_SUITE  = $(TEST_COMPILER_DIR)/llvm-test-suite
COMPILER_RT_BUILTINS = build/compiler-rt-builtins/lib/linux/libclang_rt.builtins-penumbra.a
OPT             ?= -O2

# We only run UnitTests and Regression for now
# to keep the runtime reasonable.

# Exclude patterns live in test/compiler/excludes.txt (organized by
# category with comments).  See that file for what's excluded and why.
COMPILER_EXCLUDES_FILE := $(TEST_COMPILER_DIR)/excludes.txt
COMPILER_TEST_FLAGS_FILE := $(TEST_COMPILER_DIR)/test-flags.txt

# If COMPILER_TESTS is set, run only those specific files.  Otherwise
# walk the full UnitTests and Regression trees.
ifdef COMPILER_TESTS
COMPILER_TEST_ARGS := $(foreach t,$(COMPILER_TESTS),--test-file "$(t)")
else
COMPILER_TEST_ARGS := \
	--test-dir "$(LLVM_TEST_SUITE)/UnitTests" \
	--test-dir "$(LLVM_TEST_SUITE)/Regression" \
	--test-dir "$(LLVM_TEST_SUITE)/Benchmarks/Stanford"
endif

.PHONY: test-compiler
test-compiler: $(ISS)
	@$(PYTHON) $(TEST_COMPILER_DIR)/run-tests.py \
		$(COMPILER_TEST_ARGS) \
		"--opt=$(OPT)" \
		--harness-dir "$(HARNESS_DIR)" \
		--build-dir "$(BUILD_DIR)/test-compiler" \
		--iss "./$(ISS)" \
		--cc "$(CC)" \
		--objcopy "$(OBJCOPY)" \
		--bin2hex "sw/tools/bin2hex.py" \
		--builtins "$(COMPILER_RT_BUILTINS)" \
		--resource-dir "$$($(CC) -print-resource-dir)" \
		--exclude-file "$(COMPILER_EXCLUDES_FILE)" \
		--flags-file "$(COMPILER_TEST_FLAGS_FILE)" \
		--report "$(BUILD_DIR)/test-compiler-report.txt" \
		--jobs $$(nproc)

# ── Interactive simulation (ISS — fast, instruction-level) ─────
# Builds boot ROM and runs through the ISS. No Docker needed.
# Usage: make simulate                    (interactive, default)
#        make simulate SDCARD=build/boot.img
#        make simulate TRACE=build/trace.log
#        make simulate RAW=1                  (full raw TTY for job control)
#        make simulate LLVM_PREFIX=/other/llvm/build
ISS = sw/sim/penumbra-iss

.PHONY: simulate
simulate: $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(ISS) program.hex $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE)) $(if $(RAW),+raw)

$(ISS): sw/sim/penumbra_iss.cpp
	@$(MAKE) -C sw/sim

# ── RTL simulation (Verilator — cycle-accurate, slow) ─────────
# Full RTL simulation via Docker/Verilator. Use for hardware
# verification or when cycle-accurate behavior matters.
# Usage: make simulate-rtl
#        make simulate-rtl INTERACTIVE=0   (non-interactive, piped input)
#        make simulate-rtl SDCARD=build/boot.img
#        make simulate-rtl TRACE=build/trace.log TRACE_WINDOW=1000000
#                                              (rolling last-N-instructions trace)
#        make simulate-rtl HALT_ON='user signal:'
#                                              (auto-exit on UART pattern match)
#        make simulate-rtl STDIN_FILE=boot.txt
#                                              (deterministic keystroke replay
#                                               from file at fixed cycle cadence,
#                                               then falls back to live stdin)
ifeq ($(INTERACTIVE),0)
DOCKER_RUN_IT = docker run --rm -u $(shell id -u):$(shell id -g) -i -v $(CURDIR):/work -w /work
else
DOCKER_RUN_IT = docker run --rm -u $(shell id -u):$(shell id -g) -it -v $(CURDIR):/work -w /work
endif

.PHONY: simulate-rtl
simulate-rtl:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim_interactive.verilator \
		-o ../Vmachine_sim_interactive \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_interactive.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@$(DOCKER_RUN_IT) --entrypoint ./$(BUILD_DIR)/Vmachine_sim_interactive $(DOCKER_IMAGE) $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE)) $(if $(TRACE_WINDOW),+trace_window=$(TRACE_WINDOW)) $(if $(HALT_ON),'+halt_on=$(HALT_ON)') $(if $(STDIN_FILE),+stdin_file=$(STDIN_FILE))

# ── SD card image ──────────────────────────────────────────────
# Builds SD image with bootloader, kernel, and optionally a root filesystem.
# Prerequisites: kernel and bootloader already built (see CLAUDE.md).
# For rootfs: run `build.sh distribution` first.
#
# Usage:
#   make sdimage                      — boot partition only (FAT32)
#   make sdimage-rootfs               — boot + FFS root (minimal rescue)
#   make sdimage-rootfs ROOTFS_FULL=1 — boot + FFS root (full distribution)
SDIMAGE    ?= $(BUILD_DIR)/boot.img
BOOT_ELF   := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF
KERNEL     := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/compile/MINIMAL/netbsd
DESTDIR    := $(BUILD_DIR)/netbsd-dest
ROOTFS_IMG := $(BUILD_DIR)/rootfs.img

.PHONY: sdimage
sdimage:
	@sw/tools/mksdimage.sh -o $(SDIMAGE) -2 $(BOOT_ELF) -k $(KERNEL) -v
	@echo "SD image: $(SDIMAGE)"

# Overlay pbench + mandelbrot binaries into /usr/local/bin/ if they've
# been built.  Full-rootfs only — minimal mode (rescue + lib + etc)
# intentionally excludes userland binaries.
NETBSD_BENCH_DIR := $(BUILD_DIR)/netbsd-bench
NETBSD_BENCH_OVERLAYS :=
ifeq ($(ROOTFS_FULL),1)
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/pbench),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/pbench:/usr/local/bin/pbench
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/pbench-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/pbench-static:/usr/local/bin/pbench-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/mandelbrot),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/mandelbrot:/usr/local/bin/mandelbrot
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/mandelbrot-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/mandelbrot-static:/usr/local/bin/mandelbrot-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/julia),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/julia:/usr/local/bin/julia
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/julia-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/julia-static:/usr/local/bin/julia-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/plasma),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/plasma:/usr/local/bin/plasma
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/plasma-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/plasma-static:/usr/local/bin/plasma-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/lorenz),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/lorenz:/usr/local/bin/lorenz
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/lorenz-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/lorenz-static:/usr/local/bin/lorenz-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/shadebobs),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/shadebobs:/usr/local/bin/shadebobs
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/shadebobs-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/shadebobs-static:/usr/local/bin/shadebobs-static
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/penumbra-text),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/penumbra-text:/usr/local/bin/penumbra-text
endif
ifneq ($(wildcard $(NETBSD_BENCH_DIR)/penumbra-text-static),)
NETBSD_BENCH_OVERLAYS += -i $(NETBSD_BENCH_DIR)/penumbra-text-static:/usr/local/bin/penumbra-text-static
endif
endif

# ── Custom NetBSD userland overlays ───────────────────────────────
# Each custom utility installs into a shared overlay fake-root via its
# own `overlay' target; mkrootfs.sh -O copies the whole tree into the
# image.  Add a utility by giving it an `overlay' target and adding it
# to NETBSD_OVERLAYS.  (The NETBSD_BENCH_OVERLAYS -i list above is
# superseded by this and no longer referenced.)
OVERLAY_ROOT    := $(BUILD_DIR)/netbsd-overlay
NETBSD_OVERLAYS := benchmark-overlay penmon-overlay

.PHONY: netbsd-overlay benchmark-overlay penmon-overlay penmon
netbsd-overlay:
	rm -rf $(OVERLAY_ROOT)
	@$(MAKE) $(NETBSD_OVERLAYS)

benchmark-overlay:
	@$(MAKE) -C benchmark/netbsd-bench LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

penmon-overlay:
	@$(MAKE) -C sw/penmon LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR)) OVERLAY_ROOT=$(abspath $(OVERLAY_ROOT)) overlay

penmon:
	@$(MAKE) -C sw/penmon LLVM_PREFIX=$(LLVM_PREFIX) DESTDIR=$(abspath $(DESTDIR))

.PHONY: rootfs
rootfs: netbsd-overlay
	@sw/tools/mkrootfs.sh -d $(DESTDIR) -o $(ROOTFS_IMG) \
		-k $(KERNEL) $(if $(ROOTFS_FULL),,-m) \
		-O $(OVERLAY_ROOT) -v

.PHONY: sdimage-rootfs
sdimage-rootfs: rootfs
	@sw/tools/mksdimage.sh -o $(SDIMAGE) -2 $(BOOT_ELF) -k $(KERNEL) \
		-r $(ROOTFS_IMG) -v
	@echo "SD image: $(SDIMAGE) (with FFS root)"

# Cross-built NetBSD-hosted benchmark suite (pbench).  Builds dynamic
# and static binaries against the NetBSD sysroot.  Combine with
# `sdimage-rootfs ROOTFS_FULL=1` to overlay them into /usr/local/bin/.
#
#   make benchmark-netbsd                       — build both binaries
#   make benchmark-netbsd sdimage-rootfs ROOTFS_FULL=1
#                                                — and bake into rootfs
.PHONY: benchmark-netbsd
benchmark-netbsd:
	@$(MAKE) -C benchmark/netbsd-bench LLVM_PREFIX=$(LLVM_PREFIX) \
		DESTDIR=$(abspath $(DESTDIR))
	@echo "pbench binaries:     $(NETBSD_BENCH_DIR)/pbench{,-static}"
	@echo "mandelbrot binaries: $(NETBSD_BENCH_DIR)/mandelbrot{,-static}"
	@echo "julia binaries:      $(NETBSD_BENCH_DIR)/julia{,-static}"
	@echo "plasma binaries:     $(NETBSD_BENCH_DIR)/plasma{,-static}"
	@echo "lorenz binaries:     $(NETBSD_BENCH_DIR)/lorenz{,-static}"
	@echo "shadebobs binaries:  $(NETBSD_BENCH_DIR)/shadebobs{,-static}"
	@echo "penumbra-text bins:  $(NETBSD_BENCH_DIR)/penumbra-text{,-static}"

# ── Benchmark SD image and runners ───────────────────────────
# Builds benchmark ELFs and creates an SD image containing them.
# Usage:
#   make sdimage-bench                       — build benchmarks + SD image
#   make benchmark                           — run all benchmarks on ISS (fast)
#   make benchmark-rtl                       — run all benchmarks on Verilator (cycle-accurate)
#   make benchmark BENCH_ITERS=10            — override Dhrystone iteration count
#
# BENCH_ITERS is an opt-in passthrough.  The actual default lives in
# benchmark/Makefile (DHRYSTONE_ITERATIONS) so there's a single source
# of truth — top-level only forwards when the user supplies a value.
BENCH_IMG := $(BUILD_DIR)/bench.img

# Always rebuild the benchmark sources: they're small, compile quickly, and
# `make` can't see when the compiler itself has changed under it.
.PHONY: sdimage-bench
sdimage-bench:
	@$(MAKE) -C benchmark clean
	@$(MAKE) -C benchmark $(if $(BENCH_ITERS),DHRYSTONE_ITERATIONS=$(BENCH_ITERS)) LLVM_PREFIX=$(LLVM_PREFIX)
	@mkdir -p $(BUILD_DIR)/bench_sd
	@cp $(BUILD_DIR)/benchmark/*.ELF $(BUILD_DIR)/bench_sd/ 2>/dev/null || true
	@sw/tools/mksdimage.sh -o $(BENCH_IMG) -e $(BUILD_DIR)/bench_sd -v
	@echo "Benchmark SD image: $(BENCH_IMG)"

# List of benchmark ELF names (FAT32 8.3 format, no path).
# MEMTEST is a correctness check; MEMBENCH and DHRYSTON are perf measurements.
BENCH_ELFS := DHRYSTON.ELF MEMTEST.ELF MEMBENCH.ELF

.PHONY: benchmark
benchmark: sdimage-bench $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on ISS ═══"; \
		echo "boot sd:0,0/$$elf" | $(ISS) program.hex +sdcard=$(BENCH_IMG) \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

.PHONY: benchmark-rtl
benchmark-rtl: sdimage-bench
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim_interactive.verilator \
		-o ../Vmachine_sim_interactive \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_interactive.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on RTL sim ═══"; \
		echo "boot sd:0,0/$$elf" | \
			docker run --rm -u $(shell id -u):$(shell id -g) -i -v $(CURDIR):/work -w /work \
			--entrypoint ./$(BUILD_DIR)/Vmachine_sim_interactive \
			$(DOCKER_IMAGE) +sdcard=$(BENCH_IMG) \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

# ── FPGA build (OSS CAD Suite via Docker wrappers) ────────────
FPGA_TOOLS = hw/tools/oss-cad-suite/bin
FPGA_RTL   = hw/rtl/fpga
LPF        = hw/constraints/ulx3s_v20.lpf

# Synthesis + PnR + bitstream for a top-level module.
# Usage: make fpga TOP=ulx3s_top     (full CPU system)
# TOP has no default on purpose: an explicit choice avoids silently
# building the wrong design. fpga / flash / timing error out if unset.
ifneq ($(filter fpga flash timing,$(MAKECMDGOALS)),)
ifeq ($(strip $(TOP)),)
$(error TOP is required for '$(MAKECMDGOALS)' — e.g. make fpga TOP=ulx3s_top)
endif
endif

# Source files: simple test tops use only fpga/*.sv;
# ulx3s_top needs the full RTL (core, mmu, soc devices, io).
FPGA_SRC_SIMPLE = $(wildcard $(FPGA_RTL)/*.sv)
FPGA_SRC_FULL   = hw/rtl/common/penumbra_pkg.sv \
                  hw/rtl/io/sdram/sdram_pkg.sv \
                  $(wildcard hw/rtl/penumbra1/*.sv) \
                  $(wildcard hw/rtl/mmu/*.sv) \
                  $(wildcard hw/rtl/soc/*.sv) \
                  $(wildcard hw/rtl/io/*.sv) \
                  $(filter-out %/sdram_pkg.sv, $(wildcard hw/rtl/io/sdram/*.sv)) \
                  $(FPGA_RTL)/fpga_ram.sv $(FPGA_RTL)/ulx3s_top.sv

# ECP5 primitive stubs — for Verilator lint only, not synthesis.
FPGA_LINT_STUBS = $(FPGA_RTL)/ecp5_prim.sv

# Select source set based on TOP module
ifeq ($(TOP),ulx3s_top)
FPGA_SRC = $(FPGA_SRC_FULL)
else
FPGA_SRC = $(FPGA_SRC_SIMPLE)
endif

.PHONY: fpga flash fpga-lint

# Lint always targets the full system (ulx3s_top) regardless of TOP.
# Use Verilator --lint-only with ECP5 primitive stubs.
fpga-lint: $(FPGA_SRC_FULL) $(FPGA_LINT_STUBS)
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_FULL) $(FPGA_LINT_STUBS) --top ulx3s_top

fpga: $(BUILD_DIR)/$(TOP).bit
	@echo "Bitstream: $(BUILD_DIR)/$(TOP).bit (PHASE_DEG=$(PHASE_DEG))"

# ── SDRAM phase sweep knob ──────────────────────────────────────
# Used by ulx3s_top to set CLKOS2 phase shift (the SDRAM-clock pin
# clock).  Valid values: 0, 45, 90, 135, 180, 225, 270, 315.
# Default 180° is the step-4 baseline; the bring-up sweep iterates
# all 8 to find the centred working window.  See
# doc/internals/sdram-controller.md § Step-5 phase sweep.
PHASE_DEG ?= 180

# Stamp file invalidates downstream artefacts when PHASE_DEG changes.
# We bake the value into the filename, so switching phase makes the
# previous stamp file vanish from the dep graph and forces a rebuild
# from sv2v onward.  Old stamps are wiped on each new value so the
# build dir stays tidy.
PHASE_STAMP = $(BUILD_DIR)/.phase-$(PHASE_DEG)
$(PHASE_STAMP):
	@mkdir -p $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.phase-*
	@touch $@

# Build hex files and convert SV→V before synthesis.
# sv2v converts full SystemVerilog (module-level imports, packages)
# to Verilog-2005 that Yosys reads natively.  -D SDRAM_PHASE_DEG=N
# overrides the `define inside ulx3s_top.sv for the current build.
#
# For ulx3s_top, the microcode source and boot-ROM sources are
# inlined into the generated Verilog via inline_hex.py, so the json
# target must rebuild whenever either changes.  Use conditional
# prerequisites so other TOPs (which don't use these) aren't
# spuriously rebuilt by unrelated edits.
UCODE_SRC = hw/microcode/microcode.uasm
ROM_SRCS  = $(wildcard hw/rom/*.c hw/rom/*.h hw/rom/*.s hw/rom/*.ld hw/rom/Makefile)
$(BUILD_DIR)/$(TOP).json: $(FPGA_SRC) $(PHASE_STAMP) \
    $(if $(filter ulx3s_top,$(TOP)),$(UCODE_SRC) $(ROM_SRCS))
	@mkdir -p $(BUILD_DIR)
	$(if $(filter ulx3s_top,$(TOP)),$(UASM) hw/microcode/microcode.uasm -o microcode.hex)
	$(if $(filter ulx3s_top,$(TOP)),$(MAKE) -C hw/rom)
	$(FPGA_TOOLS)/sv2v -D SDRAM_PHASE_DEG=$(PHASE_DEG) $(FPGA_SRC) -w $(BUILD_DIR)/$(TOP)_sv2v.v
	$(if $(filter ulx3s_top,$(TOP)),python3 hw/tools/inline_hex.py $(BUILD_DIR)/$(TOP)_sv2v.v $(BUILD_DIR)/$(TOP)_sv2v.v)
	$(FPGA_TOOLS)/yosys -p "read_verilog $(BUILD_DIR)/$(TOP)_sv2v.v; synth_ecp5 -top $(TOP) -json $@"

$(BUILD_DIR)/$(TOP).config: $(BUILD_DIR)/$(TOP).json $(LPF)
	$(FPGA_TOOLS)/nextpnr-ecp5 --85k --package CABGA381 --speed 6 \
		--timing-allow-fail --lpf $(LPF) --json $< --textcfg $@ \
		--report $(BUILD_DIR)/$(TOP)_timing.json --detailed-timing-report

$(BUILD_DIR)/$(TOP).bit: $(BUILD_DIR)/$(TOP).config
	$(FPGA_TOOLS)/ecppack $< $@

flash: $(BUILD_DIR)/$(TOP).bit
	$(FPGA_TOOLS)/fujprog $<

# Pretty-print fmax + top critical paths from the nextpnr JSON report
# produced by the .config rule.  Doesn't trigger a build — operates
# on whatever the last FPGA build left in $(BUILD_DIR).  Override the
# path count via TOP_N=10.  DETAIL=1 adds a per-path module rollup
# (via timing-path.py) so you can see which subsystem owns each path.
.PHONY: timing
TOP_N ?= 5
timing:
	@hw/tools/timing-report.sh $(if $(DETAIL),--detail) $(BUILD_DIR)/$(TOP)_timing.json $(TOP_N)

# Pretty-print high-fanout nets from the yosys synth JSON.  Useful for
# diagnosing nextpnr routing-congestion failures: signals with hundreds
# to thousands of sinks routed through general fabric (rather than
# ECP5's dedicated global routing) saturate the router.  Like `timing`,
# operates on whatever the last FPGA build left behind — no rebuild.
# Override entry count via FANOUT_N=30, threshold via FANOUT_MIN=20.
.PHONY: fanout
FANOUT_N   ?= 25
FANOUT_MIN ?= 50
fanout:
	@python3 hw/tools/list_fanout.py $(BUILD_DIR)/$(TOP).json -n $(FANOUT_N) -m $(FANOUT_MIN)

# ── Cleanup ────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WAVE_DIR)
	@$(MAKE) -C sw/sim clean 2>/dev/null || true
	@$(MAKE) -C sw/init clean 2>/dev/null || true
