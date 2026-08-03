# Penumbra Minicomputer — Build System
# Usage:
#   make simulate            — build ROM + ISS, run interactively (fast)
#   make simulate-rtl        — build ROM + Verilator RTL sim (cycle-accurate)
#   make test-iss            — ISA conformance tests on the ISS (fast, no Docker)
#   make test [CORE=<gen>]   — isa/ + <gen>/ tests on RTL sim (slow, Docker)
#   make test-prog CORE=<gen> PROG=<name> — one program test
#   make sim MOD=<name>      — build & run testbench for a module
#   make smoke               — toolchain smoke test
#   make wave MOD=<name>     — open waveform in GTKWave
#   make sdimage             — build SD image with bootloader, kernel, rootfs
#   make fpga-lint           — Verilator lint check on the FPGA system
#   make fpga BOARD=<b> CORE=<gen> — synthesize + PnR + bitstream
#                              (TOP=<module> is the low-level escape hatch)
#   make flash [same vars]   — fpga + flash to the board via USB
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
                  $(if $(CORE_SUB),-Ihw/rtl/$(CORE) )-Ihw/rtl/common -Ihw/rtl/penumbra1 -Ihw/rtl/penumbra2 -Ihw/rtl/penumbra3 -Ihw/rtl/machine -Ihw/rtl/bus -Ihw/rtl/io -Ihw/rtl/io/sdram -Ihw/rtl/io/video -Ihw/rtl/io/usb -Ihw/rtl/soc -Ihw/rtl/sim

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
PKG_SV = hw/rtl/common/penumbra_pkg.sv hw/rtl/penumbra2/penumbra2_pkg.sv hw/rtl/penumbra3/penumbra3_pkg.sv hw/rtl/io/sdram/sdram_pkg.sv hw/rtl/io/video/video_pkg.sv hw/rtl/io/usb/usb_pkg.sv hw/rtl/fpga/ecp5_pll_pkg.sv

# Resolving a module by name (module sims, test-modules) must not pick up a
# variant fork, which redefines a base module under the same name in its own
# directory (e.g. penumbra2_5/). Module tests target the base modules; the
# variant forks are exercised through the full-machine sim (CORE=penumbra2_5).
MOD_FIND_PRUNE = -not -path '*/penumbra2_5/*'

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
		$(PKG_SV) $$(find hw/rtl -name '$(MOD).sv' $(MOD_FIND_PRUNE)) hw/sim/$(TB).cpp
	@# Assemble program (per-program hex under build/hex/, passed to the
	@# RTL via +rom_hex= — the root program.hex belongs to the boot ROM)
	@# and microcode for $readmemh
	@mkdir -p $(BUILD_DIR)/hex
	@if test -n "$(PROG)"; then \
		src=$$(ls hw/sim/programs/*/$(PROG).s 2>/dev/null | head -1); \
		if test -n "$$src"; then \
			$(PASM) --org 0xFFFF0000 $$src -o $(BUILD_DIR)/hex/$(PROG).hex; \
		else echo "ERROR: $(PROG).s not found under hw/sim/programs/" >&2; exit 1; fi; \
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

# ── Program test suites ────────────────────────────────────────
# Layout per doc/internals/build-system.md: isa/ holds conformance
# tests (must pass on the ISS and on every core generation's runner);
# <core>/ holds microarch-pinned regressions. Programs carry lit-style
# "; RUNNER:" / "; REQUIRES:" header tags; hw/tools/run-prog-tests.py
# scans them, skips unrunnable programs visibly, and reports.
CORE ?= penumbra2

# A CORE may name a microarch variant of a base generation as
# penumbra<n>_<sub> (e.g. penumbra2_5 = gen2.5): the same RTL fileset and
# runner config as the base, distinguished only by a build-time core
# parameter wired in below. Split CORE into base + suffix once here; a
# bare core name (penumbra1, penumbra2) is its own base, empty suffix.
CORE_BASE := $(word 1,$(subst _, ,$(CORE)))
CORE_SUB  := $(word 2,$(subst _, ,$(CORE)))

# The one genuinely per-variant fact: which value the variant parameter
# takes. Stated once per variant; bare cores get the baseline (0). Fed to
# both the sim (-D) and fpga (sv2v -D) builds as PENUMBRA_CPU_VARIANT.
CPU_VARIANT_penumbra2_5 := 1
CPU_VARIANT := $(or $(CPU_VARIANT_$(CORE)),0)

PROG_DIR    = hw/sim/programs
ISA_PROGS  := $(sort $(wildcard $(PROG_DIR)/isa/test_*.s))
# A variant inherits its base's programs and adds any of its own; the
# REQUIRES/PROVIDES tags skip base tests a variant cannot satisfy.
CORE_PROGS := $(sort $(wildcard $(PROG_DIR)/$(CORE_BASE)/test_*.s) \
                     $(wildcard $(PROG_DIR)/$(CORE)/test_*.s))
TEST_PROGS := $(ISA_PROGS) $(CORE_PROGS)

# test-prog narrows the suite to a single program.
ifdef TEST_FILTER
TEST_PROGS := $(filter %/$(TEST_FILTER).s,$(TEST_PROGS))
endif

# RTL runner configuration per core generation: the DUT module, the
# runner testbenches (first entry is the default; the rest are
# selectable via RUNNER tags), and the capability set the integration
# provides for REQUIRES tags.
# mmu-d / mmu-i are the per-side slices of mmu: data / fetch
# translation with TLB miss + protection faults and the
# FAULT_ADDR/FAULT_STATUS commit. Plain mmu is the full MMU including
# non-identity mappings (the gen2 flat memory stand-in is
# vaddr-addressed, so gen2 earns mmu with the VIPT L1's tag compare).
RUNNER_MOD_penumbra1      = machine_sim
RUNNER_TBS_penumbra1      = tb_cpu_prog
# bus-fault = the no-device access-fault return path (an unclaimed address
# traps to VEC_BUS_FAULT). bus = the SYSDEV_BUS controller device, whose
# RST/CFG_EN register test_busctl probes. The autoconfig daisy chain those
# bits drive is a wrapper/board concern that no conformance test exercises:
# gen1's machine_sim wires a chain; gen2's sim wrapper leaves busctl's
# outputs open until the device-discovery / SD path consumes them.
RUNNER_PROVIDES_penumbra1 = mmu mmu-d mmu-i cache l2 uart spi bus bus-fault machid perfctr timer irq wrspr usbhc

RUNNER_MOD_penumbra2      = machine_penumbra2_sim
RUNNER_TBS_penumbra2      = tb_penumbra2_prog tb_penumbra2_intr
# pinned-stalls: the exact cycle/stall-attribution counts the gen2 perfctr tests
# assert. A forwarding/predicting variant changes them, so it drops it (below).
RUNNER_PROVIDES_penumbra2 = mmu mmu-d mmu-i cache l2 wrspr machid perfctr timer irq uart bus bus-fault pinned-stalls

# Runner config keys on the base generation, so a variant inherits the
# DUT wrapper, testbenches, and capability set unchanged. A variant whose
# behavior diverges from its base attaches its capability delta here
# (e.g. dropping a capability whose tests it can no longer satisfy).
RUNNER_MOD      = $(RUNNER_MOD_$(CORE_BASE))
RUNNER_TBS      = $(RUNNER_TBS_$(CORE_BASE))
RUNNER_DEFAULT  = $(firstword $(RUNNER_TBS))
RUNNER_PROVIDES = $(filter-out $(RUNNER_DROPS_$(CORE)),$(RUNNER_PROVIDES_$(CORE_BASE)))

# Per-variant capability delta, filtered from the inherited set above (unset for
# a base core). gen2.5 forwards/predicts → cycle counts move, so it drops pinned-stalls.
RUNNER_DROPS_penumbra2_5 = pinned-stalls

# The ISS models the full machine: it provides every capability the
# gen1 machine does.
ISS_PROVIDES = $(RUNNER_PROVIDES_penumbra1)

# ── Run the program suites on the RTL sim ──────────────────────
# Usage: make test [CORE=penumbra2]
.PHONY: test
test:
	@test -n "$(strip $(RUNNER_MOD))" || \
		{ echo "ERROR: unknown CORE '$(CORE)' — known bases: penumbra1 penumbra2 (+ variants, e.g. penumbra2_5)"; exit 1; }
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR) $(BUILD_DIR)/hex
	@# Build each runner testbench once (binaries keyed by core + testbench:
	@# a variant sets a different PENUMBRA_CPU_VARIANT, so it needs its own)
	@for tb in $(RUNNER_TBS); do \
		$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
			-DPENUMBRA_CPU_VARIANT=$(CPU_VARIANT) \
			--top-module $(RUNNER_MOD) \
			--Mdir $(BUILD_DIR)/$(CORE)_$$tb.verilator \
			-o ../V$(CORE)_$$tb \
			$(PKG_SV) $$(find hw/rtl -name '$(RUNNER_MOD).sv') hw/sim/$$tb.cpp \
			|| exit 1; \
	done
	@# Microcode for $$readmemh (gen1's machine; harmless for other cores)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@python3 hw/tools/run-prog-tests.py --mode rtl \
		--asm "$(PASM) --org 0xFFFF0000" --hexdir $(BUILD_DIR)/hex \
		--provides "$(RUNNER_PROVIDES)" --default-runner $(RUNNER_DEFAULT) \
		$(foreach tb,$(RUNNER_TBS),--bin "$(tb)=$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(CORE)_$(tb) $(DOCKER_IMAGE)") \
		$(TEST_PROGS)

# ── One program test (porcelain over the same runner flow) ─────
# Usage: make test-prog CORE=penumbra2 PROG=test_store_squash
.PHONY: test-prog
test-prog:
	@test -n "$(strip $(filter %/$(PROG).s,$(TEST_PROGS)))" || \
		{ echo "ERROR: no $(PROG).s under $(PROG_DIR)/{isa,$(CORE)}"; exit 1; }
	@$(MAKE) --no-print-directory test TEST_FILTER=$(PROG)

# ── Run all hardware module unit tests ────────────────────────
# Each entry is "MOD" when the testbench filename is the default
# `tb_<MOD>.cpp` and the top module is `<MOD>`.  Use "MOD:TB" when
# the wrapper module name diverges from the testbench filename
# (typically because a `<thing>_test.sv` wrapper drives a smaller
# RTL module — see hw/rtl/sim/).
#
# Integration testbenches (tb_cpu_prog, tb_cpu_top, tb_cpu_mem, the
# tb_penumbra{1,2}_interactive consoles) are not listed here — they are
# covered by `make test`, `make simulate`, etc.
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
    penumbra2_if1_stage \
    penumbra2_if2_stage \
    penumbra2_id_stage \
    penumbra2_ex_stage \
    penumbra2_mem_stage \
    penumbra2_wb_stage \
    penumbra2_spr_file \
    penumbra2_vecfetch \
    penumbra2_irq \
    penumbra2_spine \
    penumbra2_fetch_buffer \
    cache_bram_vipt \
    txn_arbiter \
    fill_sequencer \
    ecp5_pll_test \
    unified_mem \
    uart \
    busctl \
    tlb \
    tlb_pinned \
    tlb_unit \
    penumbra2_tlb \
    penumbra2_tlb_unit \
    mmu \
    penumbra2_mmu \
    cpu_bus_arbiter \
    slip_rx \
    slip_tx \
    spi_fifo \
    video_timing \
    timer \
    sdram_test \
    sdram_cdc \
    cache_test:tb_cache \
    cache_vipt_test:tb_cache_vipt \
    l2_cache \
    autoconfig_test:tb_autoconfig \
    spi_test:tb_spi \
    sdram_adapter_test:tb_sdram_adapter \
    sdram_sim:tb_sdram_sim \
    penumbra3_bus_master_test:tb_penumbra3_bus_master \
    penumbra3_decode_test:tb_penumbra3_decode \
    penumbra3_fetch_buffer \
    penumbra3_if1_stage \
    penumbra3_if2_stage_test:tb_penumbra3_if2_stage \
    penumbra3_id_stage_test:tb_penumbra3_id_stage \
    penumbra3_ex_stage_test:tb_penumbra3_ex_stage \
    penumbra3_mem1_stage_test:tb_penumbra3_mem1_stage \
    penumbra3_mem2_stage_test:tb_penumbra3_mem2_stage \
    penumbra3_wb_stage_test:tb_penumbra3_wb_stage \
    penumbra3_regfile \
    penumbra3_spr_file \
    penumbra3_scratch_file \
    penumbra3_spine_test:tb_penumbra3_spine \
    penumbra3_vecfetch \
    penumbra3_irq \
    penumbra3_core \
    penumbra3_mmu \
    video_pattern_test \
    video_tmds_encoder \
    video_serializer \
    video_chain_test \
    usb_crc5 \
    usb_crc16 \
    usb_bit_stuff_tx \
    usb_nrzi_encode \
    usb_nrzi_decode \
    usb_bit_unstuff_rx \
    usb_serialize_tx \
    usb_deserialize_rx \
    usb_oversample_rx \
    usb_line_state \
    usb_rx_framing \
    usb_rx_test \
    usb_tx_framing \
    usb_tx_test \
    usb_loop_test \
    usbhc_pkt_tx \
    usbhc_pkt_rx \
    usbhc_txn \
    usbhc_frame_test:tb_usbhc_frame \
    usbhc_port \
    usbhc_mac_test:tb_usbhc_mac \
    usb_phy_pair_test:tb_usb_phy_pair \
    usbhc_stack_test:tb_usbhc_stack \
    usbhc_dev_test:tb_usbhc_dev

# Variant-fork unit tests: a fork (penumbra2_5/) shares its base module's name,
# so the name-based search in test-modules resolves to the base. Name the source
# explicitly here. Fields: top-module : testbench : source.sv
VARIANT_MODULE_TESTS = \
    penumbra2_ex_stage:tb_penumbra2_5_ex_stage:hw/rtl/penumbra2_5/penumbra2_ex_stage.sv \
    penumbra2_btb:tb_penumbra2_5_btb:hw/rtl/penumbra2_5/penumbra2_btb.sv

.PHONY: test-modules test-modules-variant
test-modules: test-modules-variant
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
			$(PKG_SV) $$(find hw/rtl -name "$$mod.sv" $(MOD_FIND_PRUNE)) hw/sim/$$tb.cpp \
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

# ── Variant-fork unit tests (explicit source — see VARIANT_MODULE_TESTS) ──
test-modules-variant:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	@for e in $(VARIANT_MODULE_TESTS); do \
		m=$${e%%:*}; r=$${e#*:}; tb=$${r%%:*}; s=$${r#*:}; \
		$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) -Ihw/rtl/penumbra2_5 --top-module $$m --Mdir $(BUILD_DIR)/$$tb.verilator -o ../V$$tb $(PKG_SV) $$s hw/sim/$$tb.cpp >/dev/null 2>&1 || { printf "  \033[31mBUILD FAIL\033[0m  %s\n" "$$tb"; exit 1; }; \
		$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$$tb $(DOCKER_IMAGE) || exit 1; \
	done

# ── Aggregate: program tests + module tests ───────────────────
# Usage: make test-all
.PHONY: test-all
test-all: test test-modules


# ── Run the conformance suite on the ISS (fast, no Docker) ────
# isa/ only: the ISS is the generation-independent ISA reference, so
# microarch-pinned suites do not gate it.
# Usage: make test-iss
.PHONY: test-iss
test-iss: $(ISS)
	@mkdir -p $(BUILD_DIR)/hex
	@python3 hw/tools/run-prog-tests.py --mode iss \
		--asm "$(PASM) --org 0xFFFF0000" --hexdir $(BUILD_DIR)/hex \
		--provides "$(ISS_PROVIDES)" --timeout 5 \
		--bin "iss=./$(ISS)" \
		$(ISA_PROGS)

# ── Compiler Correctness Tests ─────────────────────────────────
# Runs curated tests from llvm-test-suite on ISS +hosted mode.
# Usage: make test-compiler [OPT="-O2"]
TEST_COMPILER_DIR = test/compiler
HARNESS_DIR      = $(TEST_COMPILER_DIR)/harness
LLVM_TEST_SUITE  = $(TEST_COMPILER_DIR)/llvm-test-suite
OPT             ?= -O2

# Link the compiler-rt archive built at the same optimization level as
# the tests, so the runtime exercises the same codegen paths the suite
# is checking (a runtime pinned to one level masks codegen bugs at the
# others).  Archives are built per level by sw/tools/setup-compiler-rt.sh.
RT_OPT := $(filter -O%,$(OPT))
ifeq ($(RT_OPT),)
RT_OPT := -O2
endif
COMPILER_RT_BUILTINS = build/compiler-rt-builtins-$(patsubst -%,%,$(RT_OPT))/lib/linux/libclang_rt.builtins-penumbra.a

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
	--test-dir "$(LLVM_TEST_SUITE)/Benchmarks/Stanford" \
	--test-dir "$(TEST_COMPILER_DIR)/penumbra-abi"
endif

.PHONY: test-compiler
test-compiler: $(ISS)
	@test -f "$(COMPILER_RT_BUILTINS)" || { \
		echo "error: $(COMPILER_RT_BUILTINS) not found —" \
		     "build it with: sw/tools/setup-compiler-rt.sh $(RT_OPT)"; \
		exit 1; }
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

# PIC leg: same harness, tests compiled -fPIC and linked static at a
# fixed address (lld resolves the GOT at link time — no runtime
# relocator), so GOT-indirect global/jump-table/block-address codegen
# is exercised that the default static suite never reaches.  Curated
# tests live in test/compiler/penumbra-pic/.  Disjoint from the main
# suite — its excludes/flags files don't apply.
COMPILER_PIC_DIR := $(TEST_COMPILER_DIR)/penumbra-pic

.PHONY: test-compiler-pic
test-compiler-pic: $(ISS)
	@test -f "$(COMPILER_RT_BUILTINS)" || { \
		echo "error: $(COMPILER_RT_BUILTINS) not found —" \
		     "build it with: sw/tools/setup-compiler-rt.sh $(RT_OPT)"; \
		exit 1; }
	@$(PYTHON) $(TEST_COMPILER_DIR)/run-tests.py \
		--test-dir "$(COMPILER_PIC_DIR)" \
		"--opt=$(OPT)" \
		--pic \
		--harness-dir "$(HARNESS_DIR)" \
		--build-dir "$(BUILD_DIR)/test-compiler-pic" \
		--iss "./$(ISS)" \
		--cc "$(CC)" \
		--objcopy "$(OBJCOPY)" \
		--bin2hex "sw/tools/bin2hex.py" \
		--builtins "$(COMPILER_RT_BUILTINS)" \
		--resource-dir "$$($(CC) -print-resource-dir)" \
		--report "$(BUILD_DIR)/test-compiler-pic-report.txt" \
		--jobs $$(nproc)

# ── Interactive simulation (ISS — fast, instruction-level) ─────
# Builds boot ROM and runs through the ISS. No Docker needed.
# Usage: make simulate                    (interactive, default)
#        make simulate SDCARD=build/boot.img
#        make simulate TRACE=build/trace.log
#        make simulate TRACE=build/t.log TRACE_WINDOW=1000000 HALT_ON=DBLFLT
#        make simulate RAW=1                  (full raw TTY for job control)
#        make simulate LLVM_PREFIX=/other/llvm/build
ISS = sw/sim/penumbra-iss

.PHONY: simulate
simulate: $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(ISS) program.hex $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(USBDEV),+usbdev=$(USBDEV)) $(if $(TRACE),+trace=$(TRACE)) $(if $(TRACE_WINDOW),+trace_window=$(TRACE_WINDOW)) $(if $(HALT_ON),'+halt_on=$(HALT_ON)') $(if $(RAW),+raw)

$(ISS): sw/sim/penumbra_iss.cpp hw/sim/usb_device_sim.h hw/sim/usb_msc_sim.h
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

# simulate-rtl and benchmark-rtl are CORE-aware: both cores share the
# sim_console.cpp frontend behind a per-core shim. gen1 uses machine_sim +
# tb_penumbra1_interactive and needs microcode.hex; gen2 uses
# machine_penumbra2_sim + tb_penumbra2_interactive and has no microcode (CORE
# defaults to penumbra2, set above). Both wrappers load the boot ROM from
# program.hex (their INIT_FILE default), built by hw/rom.
ifeq ($(CORE_BASE),penumbra2)
SIMRTL_TOP    := machine_penumbra2_sim
SIMRTL_OUT    := V$(CORE)_machine_penumbra2_sim_interactive
SIMRTL_TOPSV  := hw/rtl/sim/machine_penumbra2_sim.sv
SIMRTL_TB     := hw/sim/tb_penumbra2_interactive.cpp
# gen2's machine_penumbra2_sim backs RAM with the full sdram_sim stack (32 MB),
# so a real boot image fits with no RAM parameter override. The CPU variant
# flows in via the same -D as the test/fpga builds, so penumbra2_5 reports
# "Penumbra/2.5" in the ROM banner here too.
SIMRTL_GFLAGS := -DPENUMBRA_CPU_VARIANT=$(CPU_VARIANT)
else
SIMRTL_TOP    := machine_sim
SIMRTL_OUT    := V$(CORE)_machine_sim_interactive
SIMRTL_TOPSV  := hw/rtl/sim/machine_sim.sv
SIMRTL_TB     := hw/sim/tb_penumbra1_interactive.cpp
SIMRTL_GFLAGS :=
endif

.PHONY: simulate-rtl
simulate-rtl:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) $(SIMRTL_GFLAGS) \
		--top-module $(SIMRTL_TOP) \
		--Mdir $(BUILD_DIR)/$(CORE)_$(SIMRTL_TOP)_interactive.verilator \
		-o ../$(SIMRTL_OUT) \
		$(PKG_SV) $(SIMRTL_TOPSV) $(SIMRTL_TB) hw/sim/sim_console.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
ifneq ($(CORE_BASE),penumbra2)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
endif
	@$(DOCKER_RUN_IT) --entrypoint ./$(BUILD_DIR)/$(SIMRTL_OUT) $(DOCKER_IMAGE) $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE)) $(if $(TRACE_WINDOW),+trace_window=$(TRACE_WINDOW)) $(if $(HALT_ON),'+halt_on=$(HALT_ON)') $(if $(STDIN_FILE),+stdin_file=$(STDIN_FILE)) $(if $(PIPE_LO),+pipe_lo=$(PIPE_LO)) $(if $(PIPE_HI),+pipe_hi=$(PIPE_HI))

# ── SD card image ──────────────────────────────────────────────
# Builds SD image with bootloader, kernel, and optionally a root filesystem.
# Prerequisites: kernel and bootloader already built (see CLAUDE.md).
# For rootfs: run `build.sh distribution` first.
#
# Usage:
#   make sdimage          — boot partition only (FAT32)
#   make sdimage-rootfs   — boot + full FFS root + benchmark ELFs on FAT32
SDIMAGE    ?= $(BUILD_DIR)/boot.img
BOOT_ELF   := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF
# Which kernel config the SD images carry.  GENERIC.DEBUG (consistency
# checks on) is the development default; build with KERNCONF=GENERIC
# for demo/performance images, KERNCONF=MINIMAL for the floor config.
KERNCONF   ?= GENERIC.DEBUG
KERNEL     := $(BUILD_DIR)/netbsd-obj/sys/arch/penumbra/compile/$(KERNCONF)/netbsd
DESTDIR    := $(BUILD_DIR)/netbsd-dest
ROOTFS_IMG := $(BUILD_DIR)/rootfs.img

.PHONY: sdimage
sdimage:
	@sw/tools/mksdimage.sh -o $(SDIMAGE) -2 $(BOOT_ELF) -k $(KERNEL) -v
	@echo "SD image: $(SDIMAGE)"

# Location of the cross-built NetBSD-hosted benchmark binaries (pbench +
# graphical demos).  Staged into the rootfs via the overlay step below.
NETBSD_BENCH_DIR := $(BUILD_DIR)/netbsd-bench

# ── Custom NetBSD userland overlays ───────────────────────────────
# Each custom utility installs into a shared overlay fake-root via its
# own `overlay' target; mkrootfs.sh -O copies the whole tree into the
# image.  Add a utility by giving it an `overlay' target and adding it
# to NETBSD_OVERLAYS.
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
		-k $(KERNEL) \
		-O $(OVERLAY_ROOT) -v

.PHONY: sdimage-rootfs
# Boot + full FFS root, with the bare-metal benchmark ELFs overlaid onto the
# FAT32 boot partition (-e) alongside PENBOOT.ELF — `boot sd:0,0` loads the
# bootloader by default, `boot sd:0,0/DHRYSTON.ELF` runs a benchmark.
sdimage-rootfs: rootfs bench-elfs
	@sw/tools/mksdimage.sh -o $(SDIMAGE) \
		-2 $(BOOT_ELF) \
		-k $(KERNEL) \
		-r $(ROOTFS_IMG) \
		-e $(BENCH_SD_DIR) -v
	@echo "SD image: $(SDIMAGE) (with FFS root + benchmark ELFs)"

# Cross-built NetBSD-hosted benchmark suite (pbench + graphical demos).
# Builds dynamic and static binaries against the NetBSD sysroot.
# `make sdimage-rootfs` already overlays the dynamic binaries into
# /usr/local/bin via the netbsd-overlay step; run this target on its own
# only to build/measure the binaries standalone.
#
#   make benchmark-netbsd   — build dynamic + static binaries
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
BENCH_IMG    := $(BUILD_DIR)/bench.img
BENCH_SD_DIR := $(BUILD_DIR)/bench_sd

# Build the bare-metal benchmark ELFs and stage them in BENCH_SD_DIR, ready
# to drop onto a FAT32 root — either the standalone bench image
# (sdimage-bench) or the NetBSD image's boot partition (sdimage-rootfs).
# Always rebuilds: the sources are small, compile quickly, and `make` can't
# see when the compiler itself has changed under it.
.PHONY: bench-elfs
bench-elfs:
	@$(MAKE) -C benchmark clean
	@$(MAKE) -C benchmark $(if $(BENCH_ITERS),DHRYSTONE_ITERATIONS=$(BENCH_ITERS)) LLVM_PREFIX=$(LLVM_PREFIX)
	@mkdir -p $(BENCH_SD_DIR)
	@cp $(BUILD_DIR)/benchmark/*.ELF $(BENCH_SD_DIR)/ 2>/dev/null || true

.PHONY: sdimage-bench
sdimage-bench: bench-elfs
	@sw/tools/mksdimage.sh -o $(BENCH_IMG) -e $(BENCH_SD_DIR) -v
	@echo "Benchmark SD image: $(BENCH_IMG)"

# List of benchmark ELF names (FAT32 8.3 format, no path).
# MEMTEST is a correctness check; MEMBENCH and DHRYSTON are perf measurements.
BENCH_ELFS := DHRYSTON.ELF MEMTEST.ELF MEMBENCH.ELF

.PHONY: benchmark
benchmark: sdimage-bench $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@# +halt-on-break: each benchmark ends with a BREAK "done" sentinel.
	@# Without it the interactive ISS traps that BREAK into the ROM monitor
	@# and blocks on stdin, so the loop would never reach the next ELF.
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on ISS ═══"; \
		echo "boot sd:0,0/$$elf" | $(ISS) program.hex +sdcard=$(BENCH_IMG) +halt-on-break \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

.PHONY: benchmark-rtl
benchmark-rtl: sdimage-bench
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) $(SIMRTL_GFLAGS) \
		--top-module $(SIMRTL_TOP) \
		--Mdir $(BUILD_DIR)/$(CORE)_$(SIMRTL_TOP)_interactive.verilator \
		-o ../$(SIMRTL_OUT) \
		$(PKG_SV) $(SIMRTL_TOPSV) $(SIMRTL_TB) hw/sim/sim_console.cpp
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
ifneq ($(CORE_BASE),penumbra2)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
endif
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on RTL sim ═══"; \
		echo "boot sd:0,0/$$elf" | \
			docker run --rm -u $(shell id -u):$(shell id -g) -i -v $(CURDIR):/work -w /work \
			--entrypoint ./$(BUILD_DIR)/$(SIMRTL_OUT) \
			$(DOCKER_IMAGE) +sdcard=$(BENCH_IMG) \
			|| echo "*** $$elf FAILED ***"; \
		echo ""; \
	done

# ── FPGA build (OSS CAD Suite via Docker wrappers) ────────────
FPGA_TOOLS = hw/tools/oss-cad-suite/bin
FPGA_RTL   = hw/rtl/fpga

# ── FPGA build matrix (doc/internals/build-system.md) ──────────
# Porcelain: make fpga BOARD=<board> CORE=<generation> [VARIANT=<v>]
# expands to TOP=<board>_<core>[_<variant>]_top, whose file lives at
# hw/rtl/fpga/<board>/<top>.sv and whose source set composes from the
# per-axis variables below. TOP=<module> remains the low-level escape
# hatch: registry tops get their composed sources, anything else falls
# back to the bare fpga/*.sv set (simple test tops).

# Per-axis source sets. common/ holds the shared package (must come
# first) and the generation-shared modules both cores instantiate.
SRC_COMMON = hw/rtl/common/penumbra_pkg.sv \
             $(filter-out %/penumbra_pkg.sv, $(wildcard hw/rtl/common/*.sv))

SRC_FABRIC = hw/rtl/io/sdram/sdram_pkg.sv \
             hw/rtl/io/usb/usb_pkg.sv \
             $(wildcard hw/rtl/soc/*.sv) \
             $(wildcard hw/rtl/io/*.sv) \
             $(filter-out %/sdram_pkg.sv, $(wildcard hw/rtl/io/sdram/*.sv)) \
             $(filter-out %/usb_pkg.sv, $(wildcard hw/rtl/io/usb/*.sv))

SRC_CORE_penumbra1 = $(wildcard hw/rtl/penumbra1/*.sv)
SRC_CORE_penumbra2 = hw/rtl/penumbra2/penumbra2_pkg.sv \
                     $(filter-out %/penumbra2_pkg.sv, $(wildcard hw/rtl/penumbra2/*.sv))

# gen3 (penumbra3): a full generation fork -- its own core AND its own
# on-chip memory hierarchy down to a registered bus master (see
# doc/internals/penumbra3/overview.md). Not a composition sub-variant of
# gen2: it shares only common/, the bus devices, and the board shell. pkg
# first, then the rest of the directory, mirroring gen2.
SRC_CORE_penumbra3 = hw/rtl/penumbra3/penumbra3_pkg.sv \
                     $(filter-out %/penumbra3_pkg.sv, $(wildcard hw/rtl/penumbra3/*.sv))

# gen2.5 (penumbra2_5): composition over gen2 — share gen2's leaf cells and
# fork only the integration chain into penumbra2_5/. PENUMBRA2_FORKED is the
# single list naming which gen2 files the variant overrides; the filter-out
# drops them from the gen2 set so the variant's same-named copies are the only
# definitions (a missing entry surfaces as a loud duplicate-module error).
PENUMBRA2_FORKED := penumbra2_core penumbra2_spine penumbra2_id_stage penumbra2_ex_stage penumbra2_if2_stage
SRC_CORE_penumbra2_5 = $(filter-out $(PENUMBRA2_FORKED:%=hw/rtl/penumbra2/%.sv),$(SRC_CORE_penumbra2)) \
                       $(wildcard hw/rtl/penumbra2_5/*.sv)

# CORE-aware core fileset for the fpga tops: a sub-variant overrides, a bare
# core falls back to its base set. (The sim build resolves modules from the -I
# library path instead, with the variant dir prepended above, so penumbra2_5/
# shadows penumbra2/ there without any list.)
SRC_CORE = $(or $(SRC_CORE_$(CORE)),$(SRC_CORE_$(CORE_BASE)))

# The gen2 machine's closure: the integration module plus everything it
# binds beyond the core — the sysreg devices + L2 from the shared soc/
# directory (also wildcarded by SRC_FABRIC — a top composing both should
# $(sort) its source set to dedupe). The gen2 MMU stack now lives in
# penumbra2/ (covered by SRC_CORE_penumbra2), so it is not listed here.
SRC_MACHINE_penumbra2 = hw/rtl/machine/machine_penumbra2.sv \
                        hw/rtl/soc/cpuid.sv hw/rtl/soc/machid.sv \
                        hw/rtl/soc/timer.sv hw/rtl/soc/busctl.sv \
                        hw/rtl/soc/l2_cache.sv \
                        hw/rtl/soc/cache_perfctr.sv

# Board-common helpers only — each registry entry names its own top
# file, so sibling tops never leak into each other's builds. The PLL
# config package comes first (both ULX3S tops import it to derive the
# EHXPLLL dividers from their clock targets).
SRC_BOARD_ulx3s = $(FPGA_RTL)/ecp5_pll_pkg.sv $(FPGA_RTL)/fpga_ram.sv
LPF_ulx3s       = hw/constraints/ulx3s_v20.lpf

# The registry: every valid (board, core[, variant]) top, its composed
# source set, and which hex images it embeds. Adding a combination
# means adding its top file under hw/rtl/fpga/<board>/ and its
# entries here — an unknown combination is a hard error, not a
# silently empty source list.
FPGA_TOPS = ulx3s_penumbra1_top ulx3s_penumbra2_probe_top ulx3s_penumbra2_top \
            ulx3s_penumbra2_5_top ulx3s_penumbra3_probe_memtlb_top \
            ulx3s_penumbra3_probe_issue_top ulx3s_penumbra3_probe_decode_top

FPGA_SRC_ulx3s_penumbra1_top = $(SRC_COMMON) $(SRC_CORE_penumbra1) \
                               $(SRC_FABRIC) $(SRC_BOARD_ulx3s) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra1_top.sv

# The gen2 probe is the machine (core + MMU + VIPT L1s + arbiter +
# fill sequencer + L2) against a small BRAM bus memory — the timing
# instrument that puts the IF2 tag-compare / way-mux path and the rest
# of the memory system in front of nextpnr (no board fabric/devices).
FPGA_SRC_ulx3s_penumbra2_probe_top = $(SRC_COMMON) $(SRC_CORE) \
                                     $(SRC_MACHINE_penumbra2) \
                                     hw/rtl/sim/unified_bus_mem.sv \
                                     $(FPGA_RTL)/ulx3s/ulx3s_penumbra2_probe_top.sv

# The gen2 full board system: machine_penumbra2 wired to real board fabric
# (SDRAM v2 stack + boot ROM + UART + SPI/autoconfig). SRC_FABRIC already
# wildcards mmu/ + soc/ + io/, so it covers the machine's whole closure and
# the peripherals; only machine_penumbra2.sv itself is added on top. No
# $(sort) needed — the per-axis sets keep their package files first.
FPGA_SRC_ulx3s_penumbra2_top = $(SRC_COMMON) $(SRC_CORE) \
                               $(SRC_FABRIC) $(SRC_BOARD_ulx3s) \
                               hw/rtl/machine/machine_penumbra2.sv \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra2_top.sv

# gen3 Phase-0 probe P0.3: the MEM1/MEM2 + BRAM-TLB translate cone in front
# of nextpnr (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_memtlb).
# Core fileset + the board shell; no fabric/devices.
FPGA_SRC_ulx3s_penumbra3_probe_memtlb_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_memtlb_top.sv

# gen3 Phase-0 probe P0.1: the back-end stall cone (scoreboard +
# load-completion + issue gate) in front of nextpnr
# (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_issue).
FPGA_SRC_ulx3s_penumbra3_probe_issue_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_issue_top.sv

# gen3 Phase-0 probe P0.2: the decode->issue cone (registered bundle ->
# regmap -> scoreboard -> issue) in front of nextpnr
# (make timing BOARD=ulx3s CORE=penumbra3 VARIANT=probe_decode).
FPGA_SRC_ulx3s_penumbra3_probe_decode_top = $(SRC_COMMON) $(SRC_CORE_penumbra3) \
                               $(FPGA_RTL)/ulx3s/ulx3s_penumbra3_probe_decode_top.sv

# Tops that embed the boot ROM and/or microcode: their hex images are
# generated before synthesis and inlined by inline_hex.py.
# gen2 embeds the boot ROM (no microcode — the gen2 core is hardwired).
FPGA_ROM_TOPS   = ulx3s_penumbra1_top ulx3s_penumbra2_top ulx3s_penumbra2_5_top
FPGA_UCODE_TOPS = ulx3s_penumbra1_top

# Per-top default nextpnr placement seed. Some board/core[/variant]
# combinations only close timing on a particular seed; pin it here so a
# plain `make fpga BOARD=… CORE=…` reproduces the good placement without
# anyone having to remember the number. Keyed by the artifact TOP name
# (like FPGA_ROM_TOPS), so a microarch variant can pin its own seed
# distinct from its base. Resolution (NEXTPNR_SEED ?= …, below the TOP
# derivation): an explicit NEXTPNR_SEED on the command line or environment
# always wins; `NEXTPNR_SEED=` forces a random placement; a top with no
# entry here behaves as before (no --seed passed). gen1 needs none — it
# runs at a lower clock where fmax is not the binding constraint.
DEFAULT_SEED_ulx3s_penumbra2_top   := 9
DEFAULT_SEED_ulx3s_penumbra2_5_top := 9


# BOARD/CORE porcelain → TOP derivation (CORE defaults to penumbra2
# in the test-suite section above).
ifneq ($(strip $(BOARD)),)
TOP := $(BOARD)_$(CORE)$(if $(VARIANT),_$(VARIANT))_top
# A microarch variant (e.g. penumbra2_5) is the SAME RTL top as its base,
# built with the variant parameter set. TOP names the artifact (.bit /
# .json); TOP_MODULE names the module yosys actually synthesizes.
TOP_MODULE := $(BOARD)_$(CORE_BASE)$(if $(VARIANT),_$(VARIANT))_top
endif

# Escape hatch (TOP=<module> directly): the artifact is its own module.
TOP_MODULE := $(if $(strip $(TOP_MODULE)),$(TOP_MODULE),$(TOP))

# Effective PnR seed: an explicit NEXTPNR_SEED (command line / environment)
# wins via ?=; otherwise fall back to this top's pinned default, if any
# (empty default ⇒ no --seed, the historical behavior).
NEXTPNR_SEED ?= $(DEFAULT_SEED_$(TOP))

ifneq ($(filter fpga flash timing,$(MAKECMDGOALS)),)
ifeq ($(strip $(TOP)),)
$(error BOARD/CORE or TOP is required for '$(MAKECMDGOALS)' — e.g. make fpga BOARD=ulx3s CORE=penumbra1)
endif
ifneq ($(strip $(BOARD)),)
ifeq ($(filter $(TOP),$(FPGA_TOPS)),)
$(error Unknown combination BOARD=$(BOARD) CORE=$(CORE)$(if $(VARIANT), VARIANT=$(VARIANT)) — known tops: $(FPGA_TOPS))
endif
endif
endif

# Source-set and constraints selection. Simple test tops (no registry
# entry) use only fpga/*.sv; unknown boards fall back to the ULX3S
# constraints file.
FPGA_SRC_SIMPLE = $(wildcard $(FPGA_RTL)/*.sv)
# A variant artifact has no source set of its own — it falls back to its
# base module's (TOP_MODULE), so the gen2.5 top reuses gen2's file list.
FPGA_SRC = $(if $(FPGA_SRC_$(TOP)),$(FPGA_SRC_$(TOP)),$(if $(FPGA_SRC_$(TOP_MODULE)),$(FPGA_SRC_$(TOP_MODULE)),$(FPGA_SRC_SIMPLE)))
LPF      = $(if $(LPF_$(BOARD)),$(LPF_$(BOARD)),hw/constraints/ulx3s_v20.lpf)

# Optional per-top design-constraint overlay (floorplan / timing tuning),
# layered on top of the vendor board LPF via a second --lpf. The vendor LPF
# stays board truth and is never edited; the overlay holds only design intent
# (REGION / UGROUP / FREQUENCY NET) and never pin LOCATEs or IOBUFs — a file
# that can't speak that vocabulary can't misclaim a pin. Keyed per-top so a
# floorplan travels with its registry entry, like its source set; tops with
# no entry get a byte-identical nextpnr command line.
LPF_DESIGN_ulx3s_penumbra2_top = hw/constraints/ulx3s_penumbra2_design.lpf
LPF_DESIGN = $(if $(LPF_DESIGN_$(TOP)),$(LPF_DESIGN_$(TOP)),$(LPF_DESIGN_$(TOP_MODULE)))

# Optional per-top pre-pack floorplan (nextpnr Python API). Region/placement
# constraints cannot be expressed in nextpnr's LPF — it only supports
# LOCATE COMP (exact per-cell sites), not LOCATE UGROUP / REGION — so a
# floorplan lives in a design-owned Python script run via --pre-pack: a
# separate file and mechanism from the vendor LPF, so it cannot misstate a
# board fact. Keyed per-top, like the source set and the LPF overlay.
PREPACK_ulx3s_penumbra2_top = hw/constraints/ulx3s_penumbra2_floorplan.py
PREPACK = $(if $(PREPACK_$(TOP)),$(PREPACK_$(TOP)),$(PREPACK_$(TOP_MODULE)))

# ECP5 primitive stubs — for Verilator lint only, not synthesis.
FPGA_LINT_STUBS = $(FPGA_RTL)/ecp5_prim.sv

.PHONY: fpga flash fpga-lint

# Lint covers every registered top regardless of TOP.
# Use Verilator --lint-only with ECP5 primitive stubs.
fpga-lint: $(FPGA_SRC_ulx3s_penumbra1_top) $(FPGA_SRC_ulx3s_penumbra2_probe_top) $(FPGA_SRC_ulx3s_penumbra2_top) $(FPGA_LINT_STUBS)
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra1_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra1_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra2_probe_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra2_probe_top
	$(DOCKER_RUN) $(DOCKER_IMAGE) --lint-only -Wall -Wno-fatal \
		-Wno-PINMISSING -Wno-PINCONNECTEMPTY \
		$(FPGA_SRC_ulx3s_penumbra2_top) $(FPGA_LINT_STUBS) --top ulx3s_penumbra2_top

fpga: $(BUILD_DIR)/$(TOP).bit
	@echo "Bitstream: $(BUILD_DIR)/$(TOP).bit (PHASE_DEG=$(PHASE_DEG))"

# ── SDRAM phase sweep knob ──────────────────────────────────────
# Used by the ULX3S board tops to set CLKOS2 phase shift (the
# SDRAM-clock pin clock).  Valid values: 0, 45, 90, 135, 180, 225, 270, 315.
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
# overrides the `define inside the board top for the current build.
#
# For tops listed in FPGA_ROM_TOPS / FPGA_UCODE_TOPS, the boot-ROM and
# microcode hex images are inlined into the generated Verilog via
# inline_hex.py, so the json target must rebuild whenever their
# sources change.  Conditional prerequisites keep other TOPs from
# being spuriously rebuilt by unrelated edits.
UCODE_SRC = hw/microcode/microcode.uasm
ROM_SRCS  = $(wildcard hw/rom/*.c hw/rom/*.h hw/rom/*.s hw/rom/*.ld hw/rom/Makefile)
$(BUILD_DIR)/$(TOP).json: $(FPGA_SRC) $(PHASE_STAMP) \
    $(if $(filter $(TOP),$(FPGA_UCODE_TOPS)),$(UCODE_SRC)) \
    $(if $(filter $(TOP),$(FPGA_ROM_TOPS)),$(ROM_SRCS))
	@mkdir -p $(BUILD_DIR)
	$(if $(filter $(TOP),$(FPGA_UCODE_TOPS)),$(UASM) hw/microcode/microcode.uasm -o microcode.hex)
	$(if $(filter $(TOP),$(FPGA_ROM_TOPS)),$(MAKE) -C hw/rom)
	$(FPGA_TOOLS)/sv2v -D SDRAM_PHASE_DEG=$(PHASE_DEG) -D PENUMBRA_CPU_VARIANT=$(CPU_VARIANT) $(FPGA_SRC) -w $(BUILD_DIR)/$(TOP)_sv2v.v
	$(if $(filter $(TOP),$(FPGA_ROM_TOPS) $(FPGA_UCODE_TOPS)),python3 hw/tools/inline_hex.py $(BUILD_DIR)/$(TOP)_sv2v.v $(BUILD_DIR)/$(TOP)_sv2v.v)
	$(FPGA_TOOLS)/yosys -p "read_verilog $(BUILD_DIR)/$(TOP)_sv2v.v; synth_ecp5 -top $(TOP_MODULE) -json $@"

# The nextpnr invocation shared by the normal PnR rule and the
# seed-sweep pattern rule below (device, constraints, prepack).
NEXTPNR_BASE = $(FPGA_TOOLS)/nextpnr-ecp5 --85k --package CABGA381 --speed 6 \
		--timing-allow-fail --lpf $(LPF) \
		$(if $(LPF_DESIGN),--lpf $(LPF_DESIGN)) \
		$(if $(PREPACK),--pre-pack $(PREPACK))

$(BUILD_DIR)/$(TOP).config: $(BUILD_DIR)/$(TOP).json $(LPF) $(LPF_DESIGN) $(PREPACK)
	@echo "[fpga] PnR seed: $(if $(strip $(NEXTPNR_SEED)),$(NEXTPNR_SEED)$(if $(filter $(NEXTPNR_SEED),$(DEFAULT_SEED_$(TOP))), (pinned default for $(TOP))),random — no --seed)"
	$(NEXTPNR_BASE) \
		$(if $(NEXTPNR_SEED),--seed $(NEXTPNR_SEED)) \
		--json $< --textcfg $@ \
		--write $(BUILD_DIR)/$(TOP)_routed.json \
		--report $(BUILD_DIR)/$(TOP)_timing.json --detailed-timing-report

# ── Parallel seed sweeps ────────────────────────────────────────
# Seed-tagged PnR artifacts: build/<top>.s<seed>.config places the
# shared synthesis JSON with --seed <seed> and writes a seed-tagged
# timing report alongside.  Disjoint outputs per seed let one
# `make -jN` invocation run many seeds concurrently — this is what
# hw/tools/seed-sweep.sh drives.  No routed JSON and no bitstream:
# a sweep wants timing verdicts, and the winning seed is then pinned
# as DEFAULT_SEED_<top> and rebuilt through the normal fpga target.
$(BUILD_DIR)/$(TOP).s%.config: $(BUILD_DIR)/$(TOP).json $(LPF) $(LPF_DESIGN) $(PREPACK)
	$(NEXTPNR_BASE) \
		--seed $* \
		--json $< --textcfg $@ \
		--report $(BUILD_DIR)/$(TOP).s$*_timing.json --detailed-timing-report

# Echo the resolved artifact TOP name (consumed by seed-sweep.sh).
.PHONY: print-top
print-top:
	@echo $(TOP)

$(BUILD_DIR)/$(TOP).bit: $(BUILD_DIR)/$(TOP).config
	$(FPGA_TOOLS)/ecppack $< $@

flash: $(BUILD_DIR)/$(TOP).bit
	$(FPGA_TOOLS)/fujprog $<

# Pretty-print fmax + top critical paths from the nextpnr JSON report
# produced by the .config rule.  Doesn't trigger a build — operates
# on whatever the last FPGA build left in $(BUILD_DIR).  Override the
# path count via TOP_N=10.  DETAIL=rollup adds a per-path module rollup
# (which subsystem owns each path); DETAIL=full prints the full hop-by-hop
# trace (every LUT and route).  Default DETAIL=none.
.PHONY: timing
TOP_N ?= 5
DETAIL ?= none
timing:
	@hw/tools/timing-report.sh --detail=$(DETAIL) $(BUILD_DIR)/$(TOP)_timing.json $(TOP_N)

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
