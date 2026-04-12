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
#   make fpga [TOP=<module>] — synthesize + PnR + bitstream (default: ulx3s_hello)
#   make flash [TOP=<module>]— fpga + flash to ULX3S via USB
#   make clean               — remove build artifacts

# ── Configuration ──────────────────────────────────────────────
# Docker-based Verilator (avoids host install, works on WSL2)
DOCKER_IMAGE ?= verilator/verilator:latest
DOCKER_RUN   = docker run --rm -v $(CURDIR):/work -w /work

# Verilator runs inside the container; its entrypoint IS verilator.
# For commands that aren't verilator (like running the built binary),
# we override the entrypoint.
VERILATOR_FLAGS = --cc --exe --build -Wall \
                  $(if $(VCD),--trace) \
                  -CFLAGS "-std=c++17" \
                  -Ihw/rtl/core -Ihw/rtl/bus -Ihw/rtl/mmu -Ihw/rtl/io -Ihw/rtl/soc

BUILD_DIR   = build
WAVE_DIR    = waves

# ── Smoke test ─────────────────────────────────────────────────
.PHONY: smoke
smoke: $(BUILD_DIR)/Vsmoke_adder
	@echo "── Running smoke test ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vsmoke_adder $(DOCKER_IMAGE)

$(BUILD_DIR)/Vsmoke_adder: hw/rtl/core/smoke_adder.sv hw/sim/tb_smoke_adder.cpp
	@mkdir -p $(BUILD_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--Mdir $(BUILD_DIR)/smoke_adder.verilator \
		-o ../Vsmoke_adder \
		hw/rtl/core/smoke_adder.sv hw/sim/tb_smoke_adder.cpp

# ── Generic module simulation ──────────────────────────────────
# Usage: make sim MOD=alu  (expects hw/rtl/**/alu.sv and hw/sim/tb_alu.cpp)
#        make sim MOD=machine_sim PROG=test_mem TB=tb_cpu_mem
MOD  ?=
PROG ?= test_add
TB   ?= tb_$(MOD)

# Shared package — always included. --top-module tells Verilator which
# module is the DUT (otherwise it picks the first file = the package).
PKG_SV = hw/rtl/core/penumbra_pkg.sv

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
	@# Assemble program and microcode for $readmemh
	@rm -f program.hex microcode.hex
	@if test -f hw/sim/programs/$(PROG).s; then $(PASM) --org 0xFFFF0000 hw/sim/programs/$(PROG).s -o program.hex; \
	else echo "ERROR: hw/sim/programs/$(PROG).s not found" >&2; exit 1; fi
	@if test -f hw/microcode/microcode.uasm; then $(UASM) hw/microcode/microcode.uasm -o microcode.hex; fi
	@echo "── Running $(MOD) testbench ──"
	@$(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/V$(MOD) $(DOCKER_IMAGE)

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
	@# Remove program.hex so make simulate's ROM build doesn't see stale pasm output
	@rm -f program.hex
	@pass=0; fail=0; failed=""; \
	for prog in $(TEST_PROGS); do \
		if ! $(PASM) --org 0xFFFF0000 hw/sim/programs/$$prog.s -o program.hex; then \
			printf "  \033[31mFAIL\033[0m  %s (assembler error)\n" "$$prog"; \
			fail=$$((fail + 1)); \
			failed="$$failed $$prog"; \
			continue; \
		fi; \
		if $(DOCKER_RUN) --entrypoint ./$(BUILD_DIR)/Vmachine_sim $(DOCKER_IMAGE) \
			> /dev/null 2>&1; then \
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
	rm -f program.hex; \
	if [ $$fail -gt 0 ]; then \
		echo "  *** $$fail FAILED:$$failed ***"; \
		exit 1; \
	fi

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
		r1=$$(echo "break" | timeout 5 ./$(ISS) /tmp/$$prog.hex +trace=/tmp/iss_test.log 2>/dev/null; \
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

# ── Interactive simulation (ISS — fast, instruction-level) ─────
# Builds boot ROM and runs through the ISS. No Docker needed.
# Usage: make simulate                    (interactive, default)
#        make simulate SDCARD=build/boot.img
#        make simulate TRACE=build/trace.log
#        make simulate LLVM_PREFIX=/other/llvm/build
ISS = sw/sim/penumbra-iss

.PHONY: simulate
simulate: $(ISS)
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(ISS) program.hex $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE))

$(ISS): sw/sim/penumbra_iss.cpp
	@$(MAKE) -C sw/sim

# ── RTL simulation (Verilator — cycle-accurate, slow) ─────────
# Full RTL simulation via Docker/Verilator. Use for hardware
# verification or when cycle-accurate behavior matters.
# Usage: make simulate-rtl
#        make simulate-rtl INTERACTIVE=0   (non-interactive, piped input)
#        make simulate-rtl SDCARD=build/boot.img
ifeq ($(INTERACTIVE),0)
DOCKER_RUN_IT = docker run --rm -i -v $(CURDIR):/work -w /work
else
DOCKER_RUN_IT = docker run --rm -it -v $(CURDIR):/work -w /work
endif

.PHONY: simulate-rtl
simulate-rtl:
	@mkdir -p $(BUILD_DIR) $(WAVE_DIR)
	$(DOCKER_RUN) $(DOCKER_IMAGE) $(VERILATOR_FLAGS) \
		--top-module machine_sim \
		--Mdir $(BUILD_DIR)/machine_sim_interactive.verilator \
		-o ../Vmachine_sim_interactive \
		$(PKG_SV) $$(find hw/rtl -name 'machine_sim.sv') hw/sim/tb_interactive.cpp
	@rm -f program.hex
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@$(DOCKER_RUN_IT) --entrypoint ./$(BUILD_DIR)/Vmachine_sim_interactive $(DOCKER_IMAGE) $(if $(SDCARD),+sdcard=$(SDCARD)) $(if $(TRACE),+trace=$(TRACE))

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
KERNEL     := $(BUILD_DIR)/netbsd-kernel/MINIMAL/netbsd
DESTDIR    := $(BUILD_DIR)/netbsd-dest
ROOTFS_IMG := $(BUILD_DIR)/rootfs.img

.PHONY: sdimage
sdimage:
	@sw/tools/mksdimage.sh -o $(SDIMAGE) -2 $(BOOT_ELF) -k $(KERNEL) -v
	@echo "SD image: $(SDIMAGE)"

.PHONY: rootfs
rootfs:
	@sw/tools/mkrootfs.sh -d $(DESTDIR) -o $(ROOTFS_IMG) \
		$(if $(ROOTFS_FULL),,-m) -v

.PHONY: sdimage-rootfs
sdimage-rootfs: rootfs
	@sw/tools/mksdimage.sh -o $(SDIMAGE) -2 $(BOOT_ELF) -k $(KERNEL) \
		-r $(ROOTFS_IMG) -v
	@echo "SD image: $(SDIMAGE) (with FFS root)"

# ── Benchmark SD image and runners ───────────────────────────
# Builds benchmark ELFs and creates an SD image containing them.
# Usage:
#   make sdimage-bench             — build benchmarks + SD image
#   make benchmark                 — run all benchmarks on ISS (fast)
#   make benchmark-rtl             — run all benchmarks on Verilator (cycle-accurate)
#   make benchmark BENCH_ITERS=10  — override iteration count
BENCH_IMG   := $(BUILD_DIR)/bench.img
BENCH_ITERS ?= 1000

.PHONY: sdimage-bench
sdimage-bench:
	@$(MAKE) -C benchmark DHRYSTONE_ITERATIONS=$(BENCH_ITERS) LLVM_PREFIX=$(LLVM_PREFIX)
	@mkdir -p $(BUILD_DIR)/bench_sd
	@cp $(BUILD_DIR)/benchmark/*.ELF $(BUILD_DIR)/bench_sd/ 2>/dev/null || true
	@sw/tools/mksdimage.sh -o $(BENCH_IMG) -e $(BUILD_DIR)/bench_sd -v
	@echo "Benchmark SD image: $(BENCH_IMG)"

# List of benchmark ELF names (FAT32 8.3 format, no path)
BENCH_ELFS := DHRYSTON.ELF

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
	@rm -f program.hex
	@$(MAKE) -C hw/rom LLVM_PREFIX=$(LLVM_PREFIX) CFLAGS=$(CFLAGS)
	@$(UASM) hw/microcode/microcode.uasm -o microcode.hex
	@for elf in $(BENCH_ELFS); do \
		echo "═══ Running $$elf on RTL sim ═══"; \
		echo "boot sd:0,0/$$elf" | \
			docker run --rm -i -v $(CURDIR):/work -w /work \
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
# Usage: make fpga TOP=ulx3s_hello   (simple test designs)
#        make fpga TOP=ulx3s_top     (full CPU system)
TOP ?= ulx3s_hello

# Source files: simple test tops use only fpga/*.sv;
# ulx3s_top needs the full RTL (core, mmu, soc devices, io).
FPGA_SRC_SIMPLE = $(wildcard $(FPGA_RTL)/*.sv)
FPGA_SRC_FULL   = hw/rtl/core/penumbra_pkg.sv \
                  $(filter-out %/smoke_adder.sv %/penumbra_pkg.sv, $(wildcard hw/rtl/core/*.sv)) \
                  $(wildcard hw/rtl/mmu/*.sv) \
                  hw/rtl/soc/bus_devsel.sv hw/rtl/soc/boot_rom.sv \
                  hw/rtl/soc/cache.sv \
                  hw/rtl/soc/sysid.sv hw/rtl/soc/busctl.sv hw/rtl/soc/timer.sv \
                  hw/rtl/soc/autoconfig_dev.sv \
                  hw/rtl/io/uart.sv hw/rtl/io/spi.sv hw/rtl/io/sdram.sv \
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
	@echo "Bitstream: $(BUILD_DIR)/$(TOP).bit"

# Build hex files and convert SV→V before synthesis.
# sv2v converts full SystemVerilog (module-level imports, packages)
# to Verilog-2005 that Yosys reads natively.
$(BUILD_DIR)/$(TOP).json: $(FPGA_SRC)
	@mkdir -p $(BUILD_DIR)
	$(if $(filter ulx3s_top,$(TOP)),$(UASM) hw/microcode/microcode.uasm -o microcode.hex)
	$(if $(filter ulx3s_top,$(TOP)),$(MAKE) -C hw/rom)
	$(FPGA_TOOLS)/sv2v $(FPGA_SRC) -w $(BUILD_DIR)/$(TOP)_sv2v.v
	$(if $(filter ulx3s_top,$(TOP)),python3 hw/tools/inline_hex.py $(BUILD_DIR)/$(TOP)_sv2v.v $(BUILD_DIR)/$(TOP)_sv2v.v)
	$(FPGA_TOOLS)/yosys -p "read_verilog $(BUILD_DIR)/$(TOP)_sv2v.v; synth_ecp5 -top $(TOP) -json $@"

$(BUILD_DIR)/$(TOP).config: $(BUILD_DIR)/$(TOP).json $(LPF)
	$(FPGA_TOOLS)/nextpnr-ecp5 --85k --package CABGA381 --speed 6 \
		--timing-allow-fail --lpf $(LPF) --json $< --textcfg $@

$(BUILD_DIR)/$(TOP).bit: $(BUILD_DIR)/$(TOP).config
	$(FPGA_TOOLS)/ecppack $< $@

flash: $(BUILD_DIR)/$(TOP).bit
	$(FPGA_TOOLS)/fujprog $<

# ── Cleanup ────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(WAVE_DIR)
	@$(MAKE) -C sw/sim clean 2>/dev/null || true
	@$(MAKE) -C sw/init clean 2>/dev/null || true
